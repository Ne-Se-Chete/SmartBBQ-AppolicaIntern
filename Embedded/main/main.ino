#include <HX711_ADC.h>
#include <EEPROM.h>
#include <GxEPD.h>
#include <GxGDEH0154D67/GxGDEH0154D67.h>  // 1.54" b/w 200x200, SSD1681
#include <Fonts/FreeSansBold18pt7b.h>
#include <GxIO/GxIO_SPI/GxIO_SPI.h>
#include <GxIO/GxIO.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NTPClient.h> //install this library
#include <WiFiUdp.h>
#include <esp_sleep.h> // include for deep sleep
#include <ArduinoJson.h>


#define EEPROM_SIZE 16
int GAS_BOTTLE_WEIGHT;
int GAS_MAX_WEIGHT;

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  45        /* Time ESP32 will go to sleep (in seconds) */

const char* ssid = "#########";
const char* password = "#########";
const char* grillServerName = "https://#########.bubbleapps.io/#########/api/1.1/obj/#########";
const char* grillStatus = "https://#########.bubbleapps.io/#########/api/1.1/wf/#########";
const char* gasBottleWeight = "https://#########.bubbleapps.io/#########/api/1.1/obj/#########";  // Replace with your actual endpoints


const char* bearerToken = "#########";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);  // Update every 60 seconds

GxIO_Class io(SPI, /*CS=5*/ SS, /*DC=*/ 17, /*RST=*/ 16);
GxEPD_Class display(io, /*RST=*/ 16, /*BUSY=*/ 4);

//pins:
const int HX711_dout = 22; //mcu > HX711 dout pin
const int HX711_sck = 15; //mcu > HX711 sck pin

//HX711 constructor:
HX711_ADC LoadCell(HX711_dout, HX711_sck);

const int calVal_eepromAdress = 0;
unsigned long t = 0;
float initialMeasurement = 0;
int readingsForAverage = 20;
const int serialPrintInterval = 100; //increase value to slow down serial print activity
int cooking = 0;
float avrgReading;
int hasStoppedCooking = 0;


RTC_DATA_ATTR int newWeight;
RTC_DATA_ATTR int rtcCooking = 0;
RTC_DATA_ATTR time_t start_cooking_time;
RTC_DATA_ATTR float rtcInitMeasurement = 0;

void setup() {
  Serial.begin(115200); 
  delay(10);
  Serial.println();
  initializeWifiAndTimeClient();

  GAS_BOTTLE_WEIGHT = getGasBottleWeight();
  GAS_MAX_WEIGHT = getGasMaxWeight();

  Serial.print("Max Gas Weight: ");
  Serial.println(GAS_MAX_WEIGHT);

  Serial.print("Current Gas Bottle Weight: ");
  Serial.println(GAS_BOTTLE_WEIGHT);

  delay(1000);


  Serial.println("Starting...");
  setUpDisplay();
  startLoadCell();
  if (rtcInitMeasurement == 0){
    initialMeasurement = averageReading(20);
    rtcInitMeasurement = initialMeasurement;
  }else{
    initialMeasurement = rtcInitMeasurement;
  }
  displayValue(calculateGasLeft(initialMeasurement));

  Serial.print("Initial value: ");
  Serial.println(initialMeasurement);

  // If coming back from deep sleep, restore the state
  if (rtcCooking) {
    cooking = rtcCooking;
    newWeight = averageReading(readingsForAverage);
    Serial.println("Waking up from deep sleep");
    Serial.print("Cooking state: ");
    Serial.println(cooking);

  }else if (cooking) {
    Serial.println("Going to deep sleep");
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
}

void loop() {
  if (!rtcCooking) {
    avrgReading = averageReading(readingsForAverage);
    if(avrgReading > initialMeasurement)
      initialMeasurement = avrgReading; 
    Serial.print("Average: ");
    Serial.println(avrgReading);

    if(!cooking) {
      Serial.println("NOTTTTT cooking");
      Serial.print("Initial: ");
      Serial.println(initialMeasurement);
      if(initialMeasurement - avrgReading > 8) {
        delay(2000);
        for(int i = 0; i < 3; i++) {
          avrgReading = averageReading(readingsForAverage);
          Serial.print("Average: ");
          Serial.println(avrgReading);
          if(initialMeasurement - avrgReading > 8)
            cooking++;  
        }
        if(cooking > 1) {
          Serial.println("Cooking++");
          start_cooking_time = timeClient.getEpochTime();
          post_start_cooking();
          delay(1000);
          displayValue(calculateGasLeft(avrgReading));
        } else {
          cooking = 0;
        }
      }
    }
    newWeight = avrgReading;
  }
  
  hasStoppedCooking = 0;
  
  while (cooking) {

    for(int i = 0; i < 2; i++) {
      Serial.println("Cooking");
      // esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
      // esp_deep_sleep_start();
      delay(20000);
      // Check weight after sleep period
      int tmpWeight = averageReading(readingsForAverage);
      Serial.print("NEW_WEIGHT and TEMP_WEIGHT: ");
      Serial.print(newWeight);
      Serial.print("  ");
      Serial.println(tmpWeight);
      if(newWeight - tmpWeight < 6) {
        hasStoppedCooking++;
      } else {
        newWeight = tmpWeight;
        // hasStoppedCooking = 0;
      }

      if(hasStoppedCooking > 1) {
        post_to_grill_endpoint(initialMeasurement, newWeight, start_cooking_time);
        initialMeasurement = tmpWeight;
        cooking = 0;
        rtcCooking = 0;

        post_end_cooking();
        displayValue(calculateGasLeft(newWeight));
      }
    }
     if(hasStoppedCooking <= 1) {
      // Save state and go to deep sleep
      rtcCooking = cooking;
      esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
      esp_deep_sleep_start();
    }
  }

  delay(1000);  // Short delay to prevent rapid looping
}

void calibrate() {
  Serial.println("***");
  Serial.println("Start calibration:");
  Serial.println("Place the load cell an a level stable surface.");
  Serial.println("Remove any load applied to the load cell.");
  Serial.println("Send 't' from serial monitor to set the tare offset.");

  boolean _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      if (Serial.available() > 0) {
        char inByte = Serial.read();
        Serial.println(LoadCell.getTareOffset());
        if (inByte == 't') LoadCell.tareNoDelay();
        delay(2000);
      }
    }
    if (LoadCell.getTareStatus() == true) {
      EEPROM.writeLong(9, LoadCell.getTareOffset());
      Serial.println("Tare complete");
      _resume = true;
    }
  }

  Serial.println("Now, place your known mass on the loadcell.");
  Serial.println("Then send the weight of this mass (i.e. 100.0) from serial monitor.");

  float known_mass = 0;
  _resume = false;
  while (_resume == false) {
    LoadCell.update();
    if (Serial.available() > 0) {
      known_mass = Serial.parseFloat();
      if (known_mass != 0) {
        Serial.print("Known mass is: ");
        Serial.println(known_mass);
        _resume = true;
      }
    }
  }

  LoadCell.refreshDataSet(); //refresh the dataset to be sure that the known mass is measured correct
  float newCalibrationValue = LoadCell.getNewCalibration(known_mass); //get the new calibration value

  Serial.print("New calibration value has been set to: ");
  Serial.print(newCalibrationValue);
  Serial.println(", use this as calibration value (calFactor) in your project sketch.");
  Serial.print("Save this value to EEPROM adress ");
  Serial.print(calVal_eepromAdress);
  Serial.println("? y/n");

  _resume = false;
  while (_resume == false) {
    if (Serial.available() > 0) {
      char inByte = Serial.read();
      if (inByte == 'y') {
        // EEPROM.begin(EEPROM_SIZE);
        EEPROM.writeFloat(calVal_eepromAdress, newCalibrationValue);
        EEPROM.commit();
        newCalibrationValue = EEPROM.readFloat(calVal_eepromAdress);
        Serial.print("Value ");
        Serial.print(newCalibrationValue);
        Serial.print(" saved to EEPROM address: ");
        Serial.println(calVal_eepromAdress);
        _resume = true;

      } else if (inByte == 'n') {
        Serial.println("Value not saved to EEPROM");
        _resume = true;
      }
    }
  }

  Serial.println("End calibration");
  Serial.println("***");
  Serial.println("To re-calibrate, send 'r' from serial monitor.");
  Serial.println("For manual edit of the calibration value, send 'c' from serial monitor.");
  Serial.println("***");
}

void changeFromSavedCalFactor() {
  float oldCalibrationValue = LoadCell.getCalFactor();
  long int tareValue = 0;

  Serial.println("***");
  Serial.print("Current value is: ");
  Serial.println(oldCalibrationValue);
  float newCalibrationValue;
  newCalibrationValue = EEPROM.readFloat(calVal_eepromAdress);
  tareValue = EEPROM.readLong(9);
  Serial.println(tareValue);
  Serial.print("New calibration factor is set to: ");
  Serial.println(newCalibrationValue);
  LoadCell.setCalFactor(newCalibrationValue);
  LoadCell.setTareOffset(tareValue);
  Serial.print(LoadCell.getTareOffset());
  delay(2000);
  Serial.println("End change calibration value");
  Serial.println("***");
}

void startLoadCell() {
  EEPROM.begin(EEPROM_SIZE);
  int cal_val_eeprom = 0;

  LoadCell.begin();
  //LoadCell.setReverseOutput(); //uncomment to turn a negative output value to positive
  unsigned long stabilizingtime = 2000; // preciscion right after power-up can be improved by adding a few seconds of stabilizing time
  boolean _tare = false; //set this to false if you don't want tare to be performed in the next step
  LoadCell.start(stabilizingtime, _tare);
  if (LoadCell.getTareTimeoutFlag() || LoadCell.getSignalTimeoutFlag()) {
    Serial.println("Timeout, check MCU>HX711 wiring and pin designations");
    while (1);
  } else {
    cal_val_eeprom = EEPROM.readFloat(calVal_eepromAdress);
    if(cal_val_eeprom != 0) {
        changeFromSavedCalFactor();
        Serial.println("Startup is complete");
    } else {
      Serial.println("The sensors should be calibrated!");
      while (!LoadCell.update());
        calibrate();
    }
  }
}

float averageReading(int timesToRead) {
  float sum = 0;
  static boolean newDataReady = 0;
  int tmpTimesToRead = timesToRead;
  for(int i = 0; timesToRead > 0; i++) {
    newDataReady = false;
    if (LoadCell.update()) newDataReady = true;

  // get smoothed value from the dataset:
    if (newDataReady) {
      if (millis() > t + serialPrintInterval) {
        float measurement = LoadCell.getData();
        sum = sum + measurement;
        timesToRead--;
        Serial.print("Load_cell output val: ");
        Serial.println(measurement);
        newDataReady = 0;
        t = millis();
      }
    }
  }
  // Serial.print("Sum and timesToRead: ");
  // Serial.print(sum);
  // Serial.print("  ");
  // Serial.println(tmpTimesToRead);
  // delay(1000);
  return sum/tmpTimesToRead;
}

float calculateGasLeft(float currentWeight) {
  return(currentWeight - GAS_BOTTLE_WEIGHT) / GAS_MAX_WEIGHT * 100;
}

void setUpDisplay() {
  display.init(115200);
  display.setRotation(1);
  display.setFont(&FreeSansBold18pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.fillScreen(GxEPD_WHITE);
}

void displayValue(float value) {
  display.fillScreen(GxEPD_WHITE); // set the background to white (fill the buffer with value for white)
  display.setCursor(32, 111); // set the postition to start printing text
  display.print(value, 2);
  display.print("%");
  display.update();
}

void initializeWifiAndTimeClient(){
  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Connected to Wi-Fi");

  timeClient.begin();
  while(!timeClient.update()) {
    timeClient.forceUpdate();
  }
}

void my_post_request(String jsonData, const char* serverName) {
  HTTPClient http;
  http.begin(serverName);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + bearerToken);

  int httpResponseCode = http.POST(jsonData);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println(httpResponseCode);
    Serial.println(response);
  } else {
    Serial.print("Error on sending POST: ");
    Serial.println(httpResponseCode);
  }

  http.end();
}

void post_to_grill_endpoint(float grill_start_gas, float grill_end_gas, time_t start_cooking_time) {
  // Format the current time and end time
  time_t end_cooking_time = timeClient.getEpochTime();
  float average_consumption = (grill_start_gas-grill_end_gas)/(end_cooking_time-start_cooking_time);

  char end_time[20];
  char start_time[20];
  strftime(end_time, sizeof(end_time), "%Y-%m-%d %H:%M:%S", localtime(&end_cooking_time));
  strftime(start_time, sizeof(start_time), "%Y-%m-%d %H:%M:%S", localtime(&start_cooking_time));


  // Create the JSON object
  String jsonData = String("{\"grill start gas\":") + grill_start_gas +
                    ",\"average_consumption\":" + average_consumption +
                    ",\"grill end gas\":" + grill_end_gas +
                    ",\"start time\":\"" + start_time + "\"" +
                    ",\"end time\":\"" + end_time + "\"}";

  // Send the data to the server
  if (WiFi.status() == WL_CONNECTED) {
    my_post_request(jsonData, grillServerName);
  } else {
    Serial.println("Error in WiFi connection");
  }
}

void post_start_cooking() {
  // Create the JSON object
  String jsonData = String("{\"status\": \"yes\"}");

  // Send the data to the server
  if (WiFi.status() == WL_CONNECTED) {
    my_post_request(jsonData, grillStatus);
  } else {
    Serial.println("Error in WiFi connection");
  }
}

void post_end_cooking() {
  // Create the JSON object
  String jsonData = String("{\"status\": \"no\"}");

  // Send the data to the server
  if (WiFi.status() == WL_CONNECTED) {
    my_post_request(jsonData, grillStatus);
  } else {
    Serial.println("Error in WiFi connection");
  }
}




float getGasMaxWeight() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(gasBottleWeight);
    http.addHeader("Authorization", String("Bearer ") + bearerToken);
    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        float maxWeight = doc["response"]["results"][0]["max_weight"];
        http.end();
        return maxWeight*1000;
      } else {
        Serial.print("Failed to parse JSON: ");
        Serial.println(error.c_str());
      }
    } else {
      Serial.print("HTTP GET request failed, error code: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi is not connected");
  }
  return 8000; // default value if request fails or not connected to Wi-Fi
}

float getGasBottleWeight() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(gasBottleWeight);
    http.addHeader("Authorization", String("Bearer ") + bearerToken);
    int httpResponseCode = http.GET();

    if (httpResponseCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        float weight = doc["response"]["results"][0]["weight"];
        http.end();
        return weight * 1000;
      } else {
        Serial.print("Failed to parse JSON: ");
        Serial.println(error.c_str());
      }
    } else {
      Serial.print("HTTP GET request failed, error code: ");
      Serial.println(httpResponseCode);
    }
    http.end();
  } else {
    Serial.println("WiFi is not connected");
  }
  return 8000; // default value if request fails or not connected to Wi-Fi
}