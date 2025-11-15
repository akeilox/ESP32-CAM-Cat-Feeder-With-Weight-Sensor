# Hardware Setup Guide
## ESP32-CAM Cat Feeder with Motor & Weight Sensor

Complete wiring guide for building the smart cat feeder.

---
## Recommended Enclosure Design

3D printable enclosure by guarisal at https://makerworld.com/en/models/1626909-wi-fi-automatic-dry-food-feeder-for-cats-dog

**3D Printable Enclosure Features to add:**

- Mounting points for ESP32-CAM (maybe SD card access slot)
- Cutout for camera lens
- PIR sensor window
- Load cell mounting platform
- Desiccant holder for the lid
- Kibble hopper attachment points

---

## Bill of Materials (BOM)

### Required Components

| Component | Quantity | Notes | Est. Price |
|-----------|----------|-------|------------|
| ESP32-CAM (AI-Thinker) | 1 | With OV2640 camera | $8-12 |
| FTDI USB-to-Serial (3.3V) | 1 | For programming | $5-8 |
| PIR Motion Sensor (HC-SR501) | 1 | 3.3V-5V compatible | $2-3 |
| 5V Relay Module | 1 | 1-channel, optocoupler isolated | $2-3 |
| 5V DC Gear Motor | 1 | 6V motor works too | $3-5 |
| MicroSD Card | 1 | 4GB+ (Class 10) | $5-8 |
| 5V Power Supply | 1 | 2A minimum recommended | $5-8 |

### Optional Components (Weight Sensor)

| Component | Quantity | Notes | Est. Price |
|-----------|----------|-------|------------|
| HX711 Amplifier Module | 1 | 24-bit ADC | $2-4 |
| Load Cell (5kg-10kg) | 1 | Straight bar or single point | $5-10 |
| M3 Mounting Hardware | 4 sets | Bolts, nuts, washers | $2-3 |

### Additional Items

- Jumper wires (male-to-female, male-to-male)
- Breadboard (optional, for prototyping)
- 10kΩ resistor (if using MOSFET instead of relay)
- IRLZ44N MOSFET (alternative to relay)
- 1N4007 Diode (flyback protection for motor)
- Project enclosure/3D printed case

**Total Cost:** ~$40-60 (without weight sensor), ~$50-75 (with weight sensor)

---

## Pin Connections Overview

### ESP32-CAM Pinout Reference

```
ESP32-CAM (AI-Thinker)
                    ___________
                   |           |
            GND ---|           |--- 5V
         GPIO12 ---|           |--- GND
         GPIO13 ---|           |--- GPIO15
         GPIO15 ---|           |--- GPIO14
         GPIO14 ---|  ESP32    |--- GPIO2
         GPIO2  ---|   CAM     |--- GPIO4 (Flash LED)
         GPIO4  ---|           |--- GPIO16 (not broken out)
            GND ---|           |--- 3.3V
             5V ---|___________|--- GND

SD Card Slot (1-bit mode used):
- CMD  = GPIO15
- CLK  = GPIO14  
- DATA = GPIO2
```

---

## Complete Wiring Diagram

### Main Connections

```
                                    +5V POWER SUPPLY
                                         |
                    +-----------------+--+--+
                    |                 |     |
                    |                 |     |
               ESP32-CAM          PIR      Relay
                    |            Sensor    Module
                    |                 |     |
                    +--------+        |     |
                             |        |     |
                           Jumper   Motion  Motor
                           Wires   Detect  Control
```

### Detailed Wiring

#### 1. Power Distribution

```
5V Power Supply:
├─ ESP32-CAM 5V pin
├─ PIR Sensor VCC
├─ Relay Module VCC
└─ Motor (+) via Relay

Ground (GND):
├─ ESP32-CAM GND (multiple pins)
├─ PIR Sensor GND
├─ Relay Module GND
└─ Motor (-) via Relay
```

#### 2. PIR Motion Sensor → ESP32-CAM

```
PIR HC-SR501          ESP32-CAM
    VCC     --------->  5V or 3.3V
    OUT     --------->  GPIO13
    GND     --------->  GND
```

**PIR Jumper Settings:**
- Set to "H" (repeatable trigger mode)
- Adjust sensitivity pot (middle position to start)
- Adjust time delay pot (minimum to start)

#### 3. Motor Control via Relay → ESP32-CAM

```
ESP32-CAM            Relay Module
  GPIO12  --------->  IN (Signal)
    5V    --------->  VCC
   GND    --------->  GND

Relay Contacts       5V DC Motor
   COM    --------->  Motor (+)
   NO     --------->  +5V Power
   NC     --------->  (not used)
                      
Motor (-)  --------->  GND (Power Supply)
```

**Alternative: MOSFET Circuit (more efficient)**

```
ESP32-CAM                          Motor
                                     |
  GPIO12  -----+                    (+)
               |                     |
              Gate                   |
               |                     |
           +---+---+                 |
           | MOSFET|                 |
           | (N-CH)|                 |
           +---+---+                 |
               |                     |
             Drain -----------------(-)
               |
            Source
               |
    10kΩ      GND
     |         |
    +-+--------+
     
Flyback Diode (1N4007):
Place across motor terminals (cathode to +)
```

#### 4. HX711 Load Cell (Optional) → ESP32-CAM

```
HX711 Module         ESP32-CAM
    VCC    --------->  5V
    GND    --------->  GND
    DT     --------->  GPIO14
    SCK    --------->  GPIO15

Load Cell Colors     HX711 Pads
  Red wire  -------->  E+ (Excitation+)
  Black     -------->  E- (Excitation-)
  White     -------->  A- (Signal-)
  Green     -------->  A+ (Signal+)
```

**Load Cell Mounting:**
```
[Fixed Base] ----[Load Cell]---- [Food Container]
      |               |                  |
   Screw         Sense Flex          Weight
   Mount          Point              Applied
```

---

## Complete Connection Table

| ESP32-CAM Pin | Connected To | Purpose |
|---------------|--------------|---------|
| 5V | Power Supply +5V | Power input |
| GND (multiple) | Common ground | Ground |
| GPIO13 | PIR Sensor OUT | Motion detection |
| GPIO12 | Relay IN (or MOSFET Gate) | Motor control |
| GPIO14 | HX711 DT (optional) | Weight sensor data |
| GPIO15 | HX711 SCK (optional) | Weight sensor clock |
| GPIO4 | Built-in Flash LED | Camera flash |
| GPIO2, GPIO14, GPIO15 | SD Card (internal) | Photo storage |

---

## Programming Connection (FTDI)

**For initial firmware upload only:**

```
FTDI Adapter         ESP32-CAM
  3.3V     -------->  3.3V (NOT 5V!)
   GND     -------->  GND
    TX     -------->  U0R (RX)
    RX     -------->  U0T (TX)

Programming Mode:
- Connect GPIO0 to GND (use jumper)
- Press RESET button
- Upload code
- Remove GPIO0 jumper
- Press RESET to run
```

**⚠️ CRITICAL:** Use 3.3V logic level from FTDI, but power ESP32-CAM from external 5V supply during upload!

---

## Assembly Steps

### Step 1: Test Components Individually

1. **Test ESP32-CAM:**
   - Flash basic "Blink" sketch
   - Verify camera works with example code
   - Test SD card read/write

2. **Test PIR Sensor:**
   - Connect and monitor Serial output
   - Verify motion detection triggers

3. **Test Motor/Relay:**
   - Manually trigger relay
   - Ensure motor runs smoothly

4. **Test HX711 (if used):**
   - Run calibration sketch
   - Verify weight readings

### Step 2: Connect Everything

1. Connect power rails (5V and GND)
2. Connect PIR sensor to GPIO13
3. Connect relay/MOSFET to GPIO12
4. Connect HX711 (if used) to GPIO14/15
5. Insert SD card
6. Apply power

### Step 3: Upload Firmware

1. Connect FTDI adapter
2. Put ESP32-CAM in programming mode (GPIO0 to GND)
3. Upload the cat feeder code
4. Remove GPIO0 jumper
5. Reset and monitor Serial output

### Step 4: Calibration

1. **PIR Sensitivity:**
   - Adjust PIR potentiometers for desired range
   - Test trigger distance

2. **Motor Timing:**
   - Adjust `MOTOR_RUN_TIME_MS` for kibble portion size
   - Test multiple runs

3. **Weight Sensor:**
   - Tare empty container (`/tare` command)
   - Place known weight
   - Adjust `CALIBRATION_FACTOR`
   - Reflash and test

---

## Power Considerations

### Power Requirements

| Component | Current Draw | Notes |
|-----------|--------------|-------|
| ESP32-CAM | 180-250mA | Peak during WiFi transmit |
| PIR Sensor | 50-65mA | Minimal when idle |
| Relay | 70-90mA | When coil energized |
| Motor | 100-500mA | Depends on load |
| HX711 | 1-2mA | Negligible |
| **Total Peak** | **~500-900mA** | Size PSU accordingly |

**Recommended:** 5V 2A power supply (with margin)

### Power Supply Options

1. **USB Wall Adapter** - 5V 2A phone charger (easy, common)
2. **DC Barrel Jack** - 5V 2A with 5.5mm barrel connector
3. **Buck Converter** - From 12V if using 12V motor
4. **Battery** - 18650/21700 cells with BMS (portable, requires charging)

---

## Troubleshooting Hardware

### ESP32-CAM Won't Upload

- ✓ Check FTDI is set to 3.3V logic
- ✓ Ensure GPIO0 is grounded during upload
- ✓ Power from 5V external supply (not FTDI)
- ✓ Press RESET after connecting GPIO0 to GND
- ✓ Use correct board: "AI Thinker ESP32-CAM" or the newer S3 variant
- ✓ Lower baud rate to 115200

### Camera Not Working

- ✓ Check camera ribbon cable is fully inserted
- ✓ Verify correct pin definitions in code
- ✓ Ensure PSRAM is enabled in Arduino IDE
- ✓ Try different `FRAMESIZE` settings

### SD Card Not Detected

- ✓ Format as FAT32
- ✓ Use quality card (Class 10)
- ✓ Check 1-bit mode is enabled
- ✓ Reseat the card

### PIR Always Triggering or Never Triggering

- ✓ Wait 30-60 seconds warm-up after power-on
- ✓ Adjust sensitivity potentiometer
- ✓ Check for air currents, sunlight, heat sources
- ✓ Verify GPIO13 connection

### Motor Not Running

- ✓ Check relay "clicks" when GPIO12 goes HIGH
- ✓ Verify motor power connections
- ✓ Test motor directly with 5V
- ✓ Check `MOTOR_RUN_TIME_MS` is not too short
- ✓ If using MOSFET, verify it's logic-level type

### HX711 No Reading / Unstable

- ✓ Check all 6 wire connections (4 load cell + 2 ESP32)
- ✓ Ensure load cell can flex freely
- ✓ Shield wires from EMI/motor noise
- ✓ Adjust `WEIGHT_SAMPLES` higher (e.g., 20)
- ✓ Run calibration with known weights

### Device Keeps Rebooting

- ✓ Insufficient power supply (upgrade to 2A)
- ✓ Brown-out detector triggered (add capacitor)
- ✓ Motor causing voltage drop (add 470µF cap)
- ✓ Check for short circuits

---

## PCB Layout Tips (Optional)

If designing a custom PCB:

1. **Separate power planes** for ESP32 and motor
2. **Add bulk capacitors** (470µF) near motor and ESP32
3. **TVS diode** on motor lines for spike protection
4. **Pull-down resistor** (10kΩ) on GPIO12 if using MOSFET
5. **Programming header** for easy FTDI connection
6. **Screw terminals** for motor and power input
7. **Status LEDs** on GPIO pins for debugging

---

## Safety Notes

⚠️ **IMPORTANT SAFETY WARNINGS:**

1. **Voltage:** Ensure ESP32-CAM gets clean 5V, not more
2. **Current:** Use properly rated wire gauge (22AWG is good)
3. **Motor Protection:** Add flyback diode across motor terminals especially if current flow >1Amp
4. **Isolation:** Keep high-current motor circuit separate from logic
5. **Enclosure:** Use non-conductive filament, PETG for the kibble holder
6. **Testing:** Always test with multimeter before applying power
7. **Polarity:** Double-check power polarity (reverse = damage)
8. **Load Cell:** Don't overload beyond rated capacity

---

## Alternative Circuit Options

### Using L293D Motor Driver

For bidirectional motor control:

```
ESP32 GPIO12 ---> IN1
ESP32 GPIO13 ---> IN2  
Motor A      ---> OUT1
Motor B      ---> OUT2
```

### Using Servo Instead of DC Motor

For auger-style dispensers:

```
ESP32 GPIO12 ---> Servo Signal (PWM)
Servo VCC    ---> 5V
Servo GND    ---> GND
```

Requires `ESP32Servo` library.

---

## Testing/Troubleshooting Checklist

Before finalizing assembly:

- [ ] All connections checked with multimeter
- [ ] Power supply voltage verified (5V)
- [ ] ESP32-CAM boots and connects to WiFi
- [ ] Camera captures images
- [ ] SD card saves photos
- [ ] PIR detects motion reliably
- [ ] Motor runs for correct duration
- [ ] HX711 provides stable weight readings
- [ ] Telegram bot responds to commands
- [ ] MQTT publishes to Home Assistant
- [ ] Web interface accessible
- [ ] Stream works at expected quality
- [ ] All mounting is secure

---

## Maintenance

**Weekly:**
- Check kibble level
- Verify motor operation

**Monthly:**
- Clean PIR sensor
- Clean camera lens
- Check wire connections
- Update firmware if available
- Calibrate weight sensor

**As Needed:**
- Replace SD card if errors occur
- Clean motor mechanism
- Recalibrate load cell if readings drift

---

## Upgrades & Modifications

**Easy Additions:**
- Temperature/humidity sensor (DHT22 on GPIO16)
- Water level sensor (ultrasonic on GPIO3)
- Status RGB LED (WS2812 on GPIO1)
- Battery backup (18650*21700 + TP4056)
- Larger motor for bigger portions
- Multiple dispensers with relay board

**Advanced:**
- Computer vision for pet recognition
- Meal portion tracking by weight delta
- Integration with pet collar RFID
- Voice control via Alexa/Google
- Solar panel charging system

---

## Resources

**Datasheets:**
- [ESP32-CAM Schematic](https://github.com/SeeedDocument/forum_doc/raw/master/reg/ESP32_CAM_V1.6.pdf)
- [HX711 Datasheet](https://cdn.sparkfun.com/datasheets/Sensors/ForceFlex/hx711_english.pdf)
- [HC-SR501 PIR Datasheet](https://www.mpja.com/download/31227sc.pdf)

**Tools:**
- [Fritzing](https://fritzing.org/) - Circuit diagrams
- [EasyEDA](https://easyeda.com/) - PCB design
- [Fusion 360](https://www.autodesk.com/products/fusion-360) - Enclosure design

**Arduino Libraries:**
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [HX711 Library](https://github.com/bogde/HX711)
- [PubSubClient](https://github.com/knolleary/pubsubclient)

---

## Support

For hardware issues:
- Check wiring against diagrams
- Measure voltages with multimeter  
- Post photos to project issues page
- Join ESP32 community forums

**Common voltage test points:**
- ESP32-CAM 5V pin: 4.8-5.2V
- ESP32-CAM 3.3V pin: 3.2-3.4V
- GPIO pins (HIGH): 3.2-3.3V
- GPIO pins (LOW): 0-0.1V
