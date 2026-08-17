pio run -e esp32Launcher  
python c:\users\kega2\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32 --port COM6 --baud 460800 write_flash 0x8000 .pio\build\esp32Launcher\partitions.bin 0x10000 .pio\build\esp32Launcher\firmware.bin
