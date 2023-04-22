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


Building
--------

Personally, I am working on **Linux Ubuntu Desktop 20.04 LTS** on a virtual machine.

First, we have to install the ESP-IDF 4.x BUILD system, which is explained very well in the following [link](https://docs.espressif.com/projects/esp-idf/en/v4.4.4/esp32s3/get-started/linux-setup.html)

I have installed the three compilation systems to have them ready:

`$ ./install.sh esp32`

`$ ./install.sh esp32s2`

`$ ./install.sh esp32s3`

Now we are going to clone the EspruinoS3 repository.

`$ git clone https://github.com/rgomezwap/EspruinoS3.git`

Whenever we start a new work session, we have to activate the ESP-IDF environment with: `$ . $HOME/esp/esp-idf/export.sh`

And now we'll BUILD to generate the binaries: **bootloader**, **partitions**, and **Espruino**.

`$ cd ~/EspruinoS3/make/esp32s3idf4/`

`$ rm -r build/`

`$ idf.py set-target esp32s3`

`$ mv sdkconfig.old sdkconfig`

`$ idf.py fullclean`

`$ idf.py build`

And if nothing fails, we already have it.

