EspruinoS3
===

**Espruino v2.16 MOD version to test ESP32-S3 and latests ESP-IDF versions**

I have created this repository with the main idea that it serves as a starting point to integrate the **ESP-IDF 4.x** and **5.x** BUILD system into the official Espruino repository.

Initially, I tried to integrate it into the official repository, but the changes in the BUILD system are too different and I was not able to do it.

To make it easier for anyone to track changes, I have maintained the folder structure of Espruino, and the files that have been modified are few. I have also left only the files that are involved in the ESP32 to make it easier.

I hope that the work I have done in these last weeks helps the community create an improved version.

**Remember that the initial objective is to make a success build and to have the resulting .BIN that boot on the board. From here, all hardware components must be tested to verify what other changes need to be made.**

To quickly test the .BIN files, they can be found in :

- [bootloader.bin](make/esp32s3idf4/build/bootloader/bootloader.bin)

- [partition-table.bin](make/esp32s3idf4/build/partition_table/partition-table.bin)

- [espruino.bin](make/esp32s3idf4/build/espruino.bin)
