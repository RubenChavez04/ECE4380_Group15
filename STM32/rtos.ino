#include <Wire.h>
#include <math.h>
#include <Adafruit_INA219.h>
#include <string.h>
#include <stdlib.h>
#include <Arduino.h>

//------------------------RTOS SETUP------------------------
//tast structure
typedef struct task {
  int state;
  unsigned long period;      
  unsigned long elapsedTime; 
  int (*Function)(int);
} task;

//periods
const unsigned long period = 1; 
const unsigned long periodUartTX = 500;
const unsigned long periodSampleINA = 1;
const unsigned long periodRelayCtrl = 1;

enum UartTX_states{UartTX_INIT, UartTX_SEND_AVG};
int UartTX(int);
enum SampleINA_states{INA219_INIT, INA219_SAMPLE, INA219_SEND_AVG, INA219_ERROR};
int SampleINA(int);
enum RelayCtrl_states{RelayCtrl_INIT, RelayCtrl_CTRL};
int RelayCtrl(int);

const unsigned int numTasks = 3;
task tasks[numTasks];

void TimerISR() {
  for (unsigned char i = 0; i < numTasks; i++) {
    if (tasks[i].elapsedTime >= tasks[i].period) {
      tasks[i].state = tasks[i].Function(tasks[i].state);
      tasks[i].elapsedTime = 0;
    }
    tasks[i].elapsedTime += period;
  }
}

//------------------------global variables------------------------

//ina sampling globals
bool SAMPLE_DONE = false;
float S1_mA_AVG = 0.0, S1_V_AVG = 0.0, S2_mA_AVG = 0.0, S2_V_AVG = 0.0;
float S1_PWR_AVG = 0.0, S2_PWR_AVG = 0.0;

//thresholds to auto disable relay
volatile float thresh1 = 0.0f;
volatile float thresh2 = 0.0f;

//relay control from esp web server
volatile bool r1, r2;

//------------------------UART Stuff------------------------
//rx buffer sizing
char rxBuf[64];
unsigned int rxLen = 0;

//uart with esp over tx and rx 1
#define ESP_SERIAL  Serial1
#define ESP_BAUD    115200

//------------------------Hardware------------------------
//relay pins
#define R1_PIN PB12
#define R2_PIN PB13

//ina current sensor init, using same i2c bus since one can have the address changed
Adafruit_INA219 ina219_1(0x40);
Adafruit_INA219 ina219_2(0x41);

HardwareTimer *Timer = new HardwareTimer(TIM2);

//------------------------Tasks------------------------

int UartTX(int state) {
  switch(state) {
    case UartTX_INIT:
      ESP_SERIAL.begin(ESP_BAUD);
      ESP_SERIAL.println("[STM] UART Started");
      state = UartTX_SEND_AVG;
      break;
    case UartTX_SEND_AVG:
      if (SAMPLE_DONE){
        ESP_SERIAL.println("[STM] Sending pwr avgs");
        ESP_SERIAL.print("T=");
        ESP_SERIAL.print(millis());
        ESP_SERIAL.print(",S1_P=");
        ESP_SERIAL.print(S1_PWR_AVG, 3);
        ESP_SERIAL.print(",S2_P=");
        ESP_SERIAL.println(S2_PWR_AVG, 3);
        SAMPLE_DONE = false;
      }
      break;
  }
  return state;
}

int SampleINA(int state) {
  float S1_mA, S1_V, S2_mA, S2_V;
  static float S1_mA_TOT, S1_V_TOT, S2_mA_TOT, S2_V_TOT;
  static int samples;
  switch(state) {
    case INA219_INIT:
      //initialize local variables
      S1_mA = 0.0f;
      S1_V = 0.0f;
      S2_mA = 0.0f;
      S2_V = 0.0f;
      S1_mA_TOT = 0.0f;
      S1_V_TOT = 0.0f;
      S2_mA_TOT = 0.0f;
      S2_V_TOT = 0.0f;
      samples = 0;
      SAMPLE_DONE = false;
      Wire.begin();
      Wire.setTimeout(3);

      //setup ina219 i2c addresses are 0x0040 and 0x0041, one ina has bridged A0 solder pads
      //also print through the esp UART connection since were using stm vlink and not a serial programmer
      if (!ina219_1.begin()) {
        state = INA219_ERROR;
        break;
      }

      if (!ina219_2.begin()) {
        state = INA219_ERROR;
        break;
      }

      state = INA219_SAMPLE;
      break;

    case INA219_SAMPLE: //get the average over 10 samples
      S1_mA = ina219_1.getCurrent_mA();
      S2_mA = ina219_2.getCurrent_mA();
      S1_V = ina219_1.getBusVoltage_V();
      S2_V = ina219_2.getBusVoltage_V();

      S1_mA_TOT += S1_mA;
      S1_V_TOT += S1_V;
      S2_mA_TOT += S2_mA;
      S2_V_TOT += S2_V;

      samples++;
      if (samples >= 50) {
        //calculate averages
        S1_mA_AVG = S1_mA_TOT / samples;
        S1_V_AVG = S1_V_TOT / samples;
        S2_mA_AVG = S2_mA_TOT / samples;
        S2_V_AVG = S2_V_TOT / samples;

        //use the averages to calculate power (W)
        S1_PWR_AVG = (S1_mA_AVG * S1_V_AVG) / 1000.0f;
        S2_PWR_AVG = (S2_mA_AVG * S2_V_AVG) / 1000.0f;

        SAMPLE_DONE = true;

        S1_mA_TOT = 0.0f;
        S1_V_TOT = 0.0f;
        S2_mA_TOT = 0.0f;
        S2_V_TOT = 0.0f;
        samples = 0;

        state = INA219_SEND_AVG;
      }
      break;

    case INA219_SEND_AVG:
        if (!SAMPLE_DONE){
          state = INA219_INIT;
        }
        break;

    case INA219_ERROR:
      ESP_SERIAL.println("[STM] ERROR INITIALIZING INAs, RETRYING");
      state = INA219_INIT;
      break;
  }
  return state;
}

//relay control and threshold setting
int RelayCtrl(int state) {
  while (ESP_SERIAL.available()) {
    char c = ESP_SERIAL.read();
    //ESP_SERIAL.print("[STM RX]"); //print to esp serial for debugging
    //ESP_SERIAL.println((int)c);
    if (c == '\n' || c == '\r') {//parse buffer on new line
      if (rxLen > 0) {
        if (rxLen >= sizeof(rxBuf)){
          rxLen = sizeof(rxBuf) - 1;
        }
        rxBuf[rxLen] = '\0';
        ESP_SERIAL.print("[STM] ");
        ESP_SERIAL.println(rxBuf);

        //relay commands
        if (!strncmp(rxBuf, "RELAY1=ON", 9)) {
          r1 = true;
        } else if (!strncmp(rxBuf, "RELAY1=OFF", 10)) {
          r1 = false;
        } else if (!strncmp(rxBuf, "RELAY2=ON", 9)) {
          r2 = true;
        } else if (!strncmp(rxBuf, "RELAY2=OFF", 10)) {
          r2 = false;
        }
        //set threshold
        else if (!strncmp(rxBuf, "THRESH1=", 8)) {
          thresh1 = atol(rxBuf + 8);
        } else if (!strncmp(rxBuf, "THRESH2=", 8)) {
          thresh2 = atol(rxBuf + 8);
        }
      }
      rxLen = 0;
    } else {
      if (rxLen < sizeof(rxBuf) - 1) {
        rxBuf[rxLen++] = c;
      }
    }
  }
  switch(state) {
    case RelayCtrl_INIT:
      pinMode(R1_PIN, OUTPUT);
      pinMode(R2_PIN, OUTPUT);

      r1 = true;
      r2 = true;

      state = RelayCtrl_CTRL;
      break;

    case RelayCtrl_CTRL:
      if ((S1_PWR_AVG * 1000) >= thresh1 && thresh1 > 0) {
        r1 = false;
      }
      if ((S2_PWR_AVG * 1000) >= thresh2 && thresh2 > 0) {
        r2 = false;
      }
      digitalWrite(R1_PIN, r1 ? LOW : HIGH);
      digitalWrite(R2_PIN, r2 ? LOW : HIGH);
      break;
  }

  return state;
}

void setup() {
  Serial.begin(115200);

  //setup tasks
  tasks[0].state = UartTX_INIT;
  tasks[0].period = periodUartTX;
  tasks[0].elapsedTime = tasks[0].period;
  tasks[0].Function = &UartTX;

  tasks[1].state = INA219_INIT;
  tasks[1].period = periodSampleINA;
  tasks[1].elapsedTime = tasks[1].period;
  tasks[1].Function = &SampleINA;

  tasks[2].state = RelayCtrl_INIT;
  tasks[2].period = periodRelayCtrl;
  tasks[2].elapsedTime = tasks[2].period;
  tasks[2].Function = &RelayCtrl;

  //setup timer ISR
  Timer->setOverflow(period * 1000, MICROSEC_FORMAT); 
  Timer->attachInterrupt(TimerISR);
  Timer->resume();
  
}

void loop() {
  
}
