#include <Arduino.h>
#include <BluetoothSerial.h>
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Serial Bluetooth not available or not enabled. It is only available for the ESP32 chip.
#endif
#include <ezButton.h>
#include <AceTMI.h>
#include <AceSegment.h>
#include <PacketSerial.h>

#define POT_PIN 35
#define JOY_X_PIN 39
#define JOY_Y_PIN 36
#define DEADBAND_THRESH 0.06
#define SWITCH_PIN 34
#define R_PIN 19
#define G_PIN 18
#define B_PIN 5
#define DIO_PIN 23
#define CLK_PIN 22
#define POT_SAMPLES 10
#define JOYSTICK_CALIBRATION_SAMPLES 32
#define UPDATE_INTERVAL 10 // how fast to send packets
#define ESTOP_PIN 32

// state machine stuff
enum ConnectionState
{
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  DISCONNECTING
};

bool estopEnabled = false;

ConnectionState connectionState = DISCONNECTED;

const unsigned long FLASH_DELAY = 250;
const unsigned long BUTTON_PRESS_DELAY = 25; // how long you need to press the button for stuff to happen

unsigned long stateStartMillis = 0;
unsigned long lastFlashMillis = 0;
static unsigned long lastSendMillis = 0;

bool ledOn = false;

const unsigned long CONNECTION_TIMEOUT = 5000;
long buttonMillis = 0;
long buttonStartMillis = 0;
bool buttonHeld = false;

// bluetooth stuff
//  Information about the device we want to connect to
BTAddress targetAddress;
int targetChannel = 0;
bool targetConfigured = false;

BTScanResults *btDeviceList = nullptr;

BluetoothSerial SerialBT;
esp_spp_sec_t sec_mask = ESP_SPP_SEC_NONE; // or ESP_SPP_SEC_ENCRYPT|ESP_SPP_SEC_AUTHENTICATE to request pincode confirmation
esp_spp_role_t role = ESP_SPP_ROLE_MASTER; // or ESP_SPP_ROLE_MASTER
PacketSerial packetSerial;                 // COBS framing for Bluetooth packets

const char *targetName = "LEANReciever";

// potentiometer
int potSamples[POT_SAMPLES];
int potIndex = 0;
long potTotal = 0;

// display
using ace_segment::Tm1637Module;
using ace_tmi::SimpleTmi1637Interface;
const uint8_t NUM_DIGITS = 4;
const uint8_t BIT_DELAY = 100;

SimpleTmi1637Interface tmiInterface(
    DIO_PIN,
    CLK_PIN,
    BIT_DELAY);

Tm1637Module<
    SimpleTmi1637Interface,
    NUM_DIGITS>
    display(tmiInterface);

// joystick
double joystickCenterX = 2048.0;
double joystickCenterY = 2048.0;
ezButton button(SWITCH_PIN);
ezButton estop(ESTOP_PIN);

// control stuff
double powerLeft = 0;
double powerRight = 0;
double sensitivity = 0;
double x = 0;
double y = 0;

void changeState(ConnectionState newState);
int readPotRollingAverage();
void displayNumber(int value);
uint8_t digitPattern(int digit);
void setLEDRed();
void setLEDGreen();
void setLEDOrange();
void setLEDOff();
void updateConnectionLED(bool force);
void startConnection();
void finishDiscovery();
void connectedLoop();
void calibrateJoystick();
void sendSoftStopClear();
void sendSoftStop();
void sendDrive();
void serviceConfiguration();
void debugController();

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting up!");
  Serial.print("setup: heap before Bluetooth = ");
  Serial.println(ESP.getFreeHeap());
  // HC-06 uses Classic Bluetooth; release unused BLE RAM.
  if (!SerialBT.begin("ESP32test", true, true))
  {
    Serial.println("========== serialBT failed!");
    abort();
  }
  Serial.print("setup: heap after Bluetooth = ");
  Serial.println(ESP.getFreeHeap());

  // Route all application data through PacketSerial, not SerialBT.write().
  packetSerial.setStream(&SerialBT);

  pinMode(R_PIN, OUTPUT);
  pinMode(G_PIN, OUTPUT);
  pinMode(B_PIN, OUTPUT);
  estop.setDebounceTime(10); // long because don't want to accidentally trigger it.
  button.setDebounceTime(5);

  // Initialize rolling average
  int initialValue = analogRead(POT_PIN);

  for (int i = 0; i < POT_SAMPLES; i++)
  {
    potSamples[i] = initialValue;
    potTotal += initialValue;
  }

  tmiInterface.begin();
  display.begin();

  display.setBrightness(7);
  changeState(DISCONNECTED);
  Serial.println("setup: finished");
}

String sendData = "Hi from esp32!\n";

void loop()
{
  sensitivity = readPotRollingAverage() / 4096.0f;
  Serial.println(sensitivity);
  button.loop();
  estop.loop();
  // sensitivity = map(readPotRollingAverage(), 0, 4096, 0, 1);
  sensitivity = readPotRollingAverage() / 4095.0f;
  // Serial.println(sensitivity);

  if (button.isPressed())
  {
    Serial.println("Connect button pressed (LOW/GND)");
    Serial.println("BUtton pressed");
    buttonStartMillis = millis();
    buttonHeld = true;
  }

  if (buttonHeld)
  {
    buttonMillis = millis() - buttonStartMillis;
  }

  if (button.isReleased())
  {
    Serial.println("Connect button released (HIGH)");
    buttonMillis = 0;
    buttonHeld = false;
  }

  displayNumber(ceil(sensitivity * 100));

  switch (connectionState)
  {

  case DISCONNECTED:
    setLEDRed();

    if (buttonMillis > BUTTON_PRESS_DELAY && (millis() - stateStartMillis) > 500)
    {
      startConnection(); // this function will set state to connecting if possible, otherwise disconnected.
      buttonMillis = 0;  // reset this just in case it gets set to disconnected
    }
    break;

  case CONNECTING:
    updateConnectionLED(true);

    // Check whether discovery has run long enough.
    if (SerialBT.connected())
    {
      estopEnabled = false;
      changeState(CONNECTED);
    }
    else if (millis() - stateStartMillis >= CONNECTION_TIMEOUT)
    {
      finishDiscovery();
    }
    break;

  case CONNECTED:
    if (!SerialBT.connected())
    {
      changeState(DISCONNECTED);
      break;
    }

    connectedLoop();

    if (buttonMillis > BUTTON_PRESS_DELAY && (millis() - stateStartMillis) > 500)
    {
      SerialBT.disconnect();
      changeState(DISCONNECTING);
      buttonMillis = 0;
    }
    break;
  case DISCONNECTING:
    if (!SerialBT.connected())
    {
      changeState(DISCONNECTED);
    }
    break;
  }
}

const uint8_t PACKET_DRIVE = 0x01;
const uint8_t PACKET_SOFT_STOP = 0x02;
const uint8_t PACKET_SOFT_STOP_CLEAR = 0x03;

// this acts as voidLoop() but only runs when connected
void connectedLoop()
{
  // handle estop no matter what if we are connected
  if (estop.isPressed())
  {
    if (estopEnabled)
    {
      sendSoftStopClear();
    }
    else
    {
      sendSoftStop();
    }
    estopEnabled = !estopEnabled;
  }
  if (estopEnabled)
  {
    if (millis() - lastSendMillis >= UPDATE_INTERVAL)
    {
      lastSendMillis = millis();
      sendSoftStop(); // Repeat at 100 Hz without flooding the Bluetooth queue.
    }
    setLEDOrange();
    return;
  }

  setLEDGreen();

  // limit this loop to a set interval
  if (millis() - lastSendMillis < UPDATE_INTERVAL)
  {
    return;
  }

  lastSendMillis = millis();

  sendDrive();
}

void calibrateJoystick()
{
  long totalX = 0;
  long totalY = 0;

  // The joystick is assumed to be resting whenever a connection is established.
  for (int i = 0; i < JOYSTICK_CALIBRATION_SAMPLES; i++)
  {
    totalX += analogRead(JOY_X_PIN);
    totalY += analogRead(JOY_Y_PIN);
  }

  joystickCenterX = totalX / double(JOYSTICK_CALIBRATION_SAMPLES);
  joystickCenterY = totalY / double(JOYSTICK_CALIBRATION_SAMPLES);
}

// Send normal drive command
void sendDrive()
{
  // read joystick
  x = (analogRead(JOY_X_PIN) - joystickCenterX) / 2048.0;
  y = (analogRead(JOY_Y_PIN) - joystickCenterY) / 2048.0;
  // apply deadband
  if (x >= 0 && x < DEADBAND_THRESH)
  {
    x = 0;
  }
  else if (x <= 0 && x > -DEADBAND_THRESH)
  {
    x = 0;
  }

  if (y >= 0 && y < DEADBAND_THRESH)
  {
    y = 0;
  }
  else if (y <= 0 && y > -DEADBAND_THRESH)
  {
    y = 0;
  }

  // apply sensitivity
  x *= sensitivity;
  y *= sensitivity;

  powerLeft = constrain(y - x, -sensitivity, sensitivity);
  powerRight = constrain(x + y, -sensitivity, sensitivity);

  // map to fixed size data format to make this hardware agnostic. Some architectures use different amounts of bytes for float, double etc.
  int16_t left = (int16_t)(constrain(powerLeft, -1.0, 1.0) * 32767);
  int16_t right = (int16_t)(constrain(powerRight, -1.0, 1.0) * 32767);

  uint8_t packet[1 + 2 * sizeof(int16_t)];
  packet[0] = PACKET_DRIVE;

  memcpy(packet + 1, &left, sizeof(left));
  memcpy(packet + 1 + sizeof(left), &right, sizeof(right));

  Serial.println("Sending drive packet");
  packetSerial.send(packet, sizeof(packet));

  // Serial.printf("Pressed: %i, Released: %i, state: %i\n", estop.isPressed(), estop.isReleased(), estop.getState());
}

// Send soft stop command
void sendSoftStop()
{
  uint8_t packet = PACKET_SOFT_STOP;
  packetSerial.send(&packet, sizeof(packet));
}

// Send soft stop command
void sendSoftStopClear()
{
  uint8_t packet = PACKET_SOFT_STOP_CLEAR;
  packetSerial.send(&packet, sizeof(packet));
}

void finishDiscovery()
{
  Serial.println("finishding discovery");
  SerialBT.discoverAsyncStop();

  if (btDeviceList == nullptr ||
      btDeviceList->getCount() == 0)
  {
    Serial.println("No devices found, disconnecting");
    changeState(DISCONNECTED);
    return;
  }

  for (int i = 0; i < btDeviceList->getCount(); i++)
  {
    BTAdvertisedDevice *device =
        btDeviceList->getDevice(i);

    String name = device->getName().c_str();

    if (name != targetName)
    {
      continue;
    }
    Serial.println("Reciever found");
    std::map<int, std::string> channels =
        SerialBT.getChannels(device->getAddress());

    if (channels.empty())
    {
      Serial.println("No SPP channel found.");
      continue;
    }

    targetAddress = device->getAddress();
    targetChannel = channels.begin()->first;
    targetConfigured = true;

    break;
  }

  if (!targetConfigured)
  {
    Serial.println("Target receiver or SPP channel not found.");
    changeState(DISCONNECTED);
    return;
  }

  Serial.printf(
      "Connecting to %s on channel %d\n",
      targetAddress.toString().c_str(),
      targetChannel);

  Serial.print("finishDiscovery: heap before connect = ");
  Serial.println(ESP.getFreeHeap());
  bool success = SerialBT.connect(
      targetAddress,
      targetChannel,
      sec_mask,
      role);

  if (success && SerialBT.connected())
  {
    changeState(CONNECTED);
  }
  else
  {
    Serial.println("Bluetooth connection failed.");
    changeState(DISCONNECTED);
  }
}

void btAdvertisedDeviceFound(BTAdvertisedDevice *pDevice)
{
  Serial.printf("Found a device asynchronously: %s\n", pDevice->toString().c_str());
}
void startConnection()
{
  changeState(CONNECTING);

  if (targetConfigured)
  {
    uint32_t reconnectStartMillis = millis();
    bool success = SerialBT.connect(
        targetAddress,
        targetChannel,
        sec_mask,
        role);

    if (success && SerialBT.connected())
    {
      changeState(CONNECTED);
    }
    else
    {
      Serial.println("Reconnect failed.");
      changeState(DISCONNECTED);
    }

    return;
  }
  // first connection
  Serial.println("Starting Bluetooth discovery...");

  // Obtain the scan-results object used by BluetoothSerial.
  btDeviceList = SerialBT.getScanResults();

  // start asynchronous scanning for devices. Some ESP32 BluetoothSerial
  // versions require an explicit callback argument.
  bool started = SerialBT.discoverAsync(btAdvertisedDeviceFound);

  if (!started)
  {
    Serial.println("Could not start Bluetooth discovery.");
    changeState(DISCONNECTED);
  }
}

void changeState(ConnectionState newState)
{
  connectionState = newState;
  stateStartMillis = millis();
  lastFlashMillis = millis();
  ledOn = false;

  Serial.print("Connection state: ");

  switch (newState)
  {
  case DISCONNECTED:
    Serial.println("DISCONNECTED");
    break;

  case CONNECTING:
    Serial.println("CONNECTING");
    break;

  case CONNECTED:
    calibrateJoystick();
    Serial.println("CONNECTED");
    break;
  case DISCONNECTING:
    Serial.println("DISCONNECTING");
  }
}

void updateConnectionLED(bool red)
{
  unsigned long now = millis();

  if (now - lastFlashMillis >= FLASH_DELAY)
  {
    lastFlashMillis = now;
    ledOn = !ledOn;
  }

  if (ledOn)
  {
    if (red)
    {
      setLEDRed();
    }
    else
    {
      setLEDGreen();
    }
  }
  else
  {
    setLEDOff();
  }
}

void setLEDRed()
{
  analogWrite(R_PIN, 255);
  analogWrite(G_PIN, 0);
  analogWrite(B_PIN, 0);
}

void setLEDOrange()
{
  analogWrite(R_PIN, 255);
  analogWrite(G_PIN, 40);
  analogWrite(B_PIN, 0);
}

void setLEDOff()
{
  analogWrite(R_PIN, 0);
  analogWrite(G_PIN, 0);
  analogWrite(B_PIN, 0);
}

void setLEDGreen()
{
  analogWrite(R_PIN, 0);
  analogWrite(G_PIN, 255);
  analogWrite(B_PIN, 0);
}

void displayOff()
{
  display.setPatternAt(0, 0);
  display.setPatternAt(1, 0);
  display.setPatternAt(2, 0);
  display.setPatternAt(3, 0);

  display.flush();
}

void displayNumber(int value)
{
  // Make sure value stays between 0 and 100
  value = constrain(value, 0, 100);

  if (value == 100)
  {
    display.setPatternAt(0, 0b00000110); // 1
    display.setPatternAt(1, 0b00111111); // 0
    display.setPatternAt(2, 0b00111111); // 0
    display.setPatternAt(3, 0);          // blank
  }
  else if (value >= 10)
  {
    int tens = value / 10;
    int ones = value % 10;

    display.setPatternAt(0, 0); // blank
    display.setPatternAt(1, digitPattern(tens));
    display.setPatternAt(2, digitPattern(ones));
    display.setPatternAt(3, 0); // blank
  }
  else
  {
    display.setPatternAt(0, 0); // blank
    display.setPatternAt(1, 0); // blank
    display.setPatternAt(2, digitPattern(value));
    display.setPatternAt(3, 0); // blank
  }

  display.flush();
}

uint8_t digitPattern(int digit)
{

  const uint8_t digits[] = {
      0b00111111, // 0
      0b00000110, // 1
      0b01011011, // 2
      0b01001111, // 3
      0b01100110, // 4
      0b01101101, // 5
      0b01111101, // 6
      0b00000111, // 7
      0b01111111, // 8
      0b01101111  // 9
  };

  return digits[digit];
}

int readPotRollingAverage()
{

  // Remove oldest value
  potTotal -= potSamples[potIndex];

  // Read new value
  int newValue = analogRead(POT_PIN);

  // Store new value
  potSamples[potIndex] = newValue;

  // Add new value
  potTotal += newValue;

  // Move to next position
  potIndex++;

  if (potIndex >= POT_SAMPLES)
  {
    potIndex = 0;
  }

  return potTotal / POT_SAMPLES;
}
