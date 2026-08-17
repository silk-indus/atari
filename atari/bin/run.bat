python c:\users\kega2\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32 --port COM6 --baud 460800 write_flash 0x8000 partitions.bin 0x10000 launcher.bin 0x90000 atari.bin

#python c:\users\kega2\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32 --port COM6 --baud 460800 write_flash 0x190000 spiffs.bin
#python c:\users\kega2\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32 --port COM6 --baud 460800 write_flash 0x1000 bootloader.bin