# sunsrise-sunset-lamp
An ESP32 S3 Zero project to build a sunrise/sunset lamp

# Web app
<img width="800" alt="image" src="https://raw.githubusercontent.com/dayeggpi/sunsrise-sunset-lamp/refs/heads/main/web_app.png">

# Lamp
<img width="800" alt="image" src="https://raw.githubusercontent.com/dayeggpi/sunsrise-sunset-lamp/refs/heads/main/lamp.jpeg">

# 3D box
<img width="800" alt="image" src="https://raw.githubusercontent.com/dayeggpi/sunsrise-sunset-lamp/refs/heads/main/3D_box.png">

# How to 

- Print the box (see in `/3D_files` folder, either the 3mf files, or adjust as per your needs using the python script in `/3D_files/Autodesk-Fusion_script`)
- Get an ESP32 S3 Zero (or equivalent)
- Compile and upload the code `sunrise_lamp.ino` to the ESP32 (don't forget to adjust the wifi SSID and password values)
- Go to your ESP32 local IP address
- Adjust the settings on the web app as needed
- Profit

# Bill of materials

- ESP 32 S3 Zero
- an LED strip and aluminum profile
- mp1584EN (or any DC-DC Step-Down 12v to 5v) : needed only if the led strip is 12v
- A push button
- a DC jack barrel

# Wiring

- Push button pin 1 goes to → ESP32 pinout 9
- Push button ping 2 goes to → ESP32 GND
- LED Strip Data In (Keep this wire as short as possible!) goes to → ESP32 Data Pin pinout 10


Following only needed if led strip is 12v :
- 12V Power + goes to → LED Strip 12V+
- 12V Power + goes to → MP1584EN IN+
- MP1584EN OUT+ (5V) goes to → ESP32 5V pin
- 12V Power GND goes to → LED Strip GND
- 12V Power GND goes to → MP1584EN IN-
- MP1584EN OUT- goes to → ESP32 GND
- (Optional but excellent for data) ESP32 GND goes to → LED Strip GND (run this wire right next to the data cable)


