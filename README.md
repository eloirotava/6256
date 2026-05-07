# SSV6256P (SSV6X5X) WiFi Driver

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) git
```

# Compilation

```bash
git repoo
make 
```

## 📥 Installation

```bash
sudo cp ./ssv6x5x-wifi.cfg /lib/firmware/
sudo cp ./ssv6x5x-sw.bin /lib/firmware/
sudo cp ./ssv6051-sw.bin /lib/firmware/
sudo cp ./ssv6x5x.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/
sudo depmod -a
sudo modprobe ssv6x5x
```
