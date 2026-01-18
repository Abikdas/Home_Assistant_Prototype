# Home_Assistant_Prototype
Its a home assistant prototype for automation of my room light access via a web app.

# Usecase
1. Rooms light control 
2. Some small project on and off access
3. Aesthetic led setup control for my desktop

# Components_used
1. Arduino based esp8266 wifi board
2. Relay 5v
3. Pcb (universal)
4. wires
5. Bc547
6. optocoupler
7. leds
8. Capacitors
9. casing
10. flyback diode

# Control access
- Via wifi 2.5Ghz only
# working principle
- The working principle is simple enough.
   
   
  The module first gets connected to the commited wifi. It can be accessed by searching the ip address via a browser. {Very importantly the phone or computer aslo has to be connected to the same wifi which the module board is connnected.}

  - The web command page ui can be changed by the arduino ide code just by changing the html program.

# Very_importnantNote
  - The Board cant be accessed if not connected to the same wifi.

# Possibilities

- We can also add some sensors and get data of the commited sensors by the web page itself.

- We can use it to control any appliance just by adding more configurations to the board.

- Adding a rasberry pi, giving access to cloud via internet and be able to get data of all the commited data managements coordinated to the server cloud.

