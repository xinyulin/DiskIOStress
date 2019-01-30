# DiskIOStress
Linux based storage device data integrity check tool. NVMe is the default I/O protocol, you can change to system I/O if needed


## Prerequisites
* Operating system:  >= Ubutnu 14.04 or similar distributions
* Kernel: >= 3.3 or NVMe built-in driver
* GCC: >= 4.0


## Build
```
make
```

## Run
sudo ./DiskIOStress [device path]

```
sudo ./DiskIOStress /dev/nvme0n1
```

## Contributing
If you find this tool is useful and would like to contribute this project, feel free to contact me via xinyu0123@gmail.com
