# SSV6256P / SSV6X5X / SX5S WiFi Driver

Driver out-of-tree enxuto para o módulo WiFi SSV6256P/SSV6X5X/SX5S.

## Dependências

```bash
sudo apt-get update
sudo apt-get install -y build-essential linux-headers-$(uname -r) git
```

## Compilação

```bash
make
```

## Instalação

```bash
sudo make install-firmware
sudo make install
sudo modprobe ssv6x5x
```

Ou, manualmente:

```bash
sudo cp ./ssv6x5x-wifi.cfg /lib/firmware/
sudo cp ./ssv6x5x-sw.bin /lib/firmware/
sudo cp ./ssv6x5x.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/
sudo depmod -a
sudo modprobe ssv6x5x
```

## Firmware

Este build usa apenas o firmware `ssv6x5x-sw.bin`. O firmware legado `ssv6051-sw.bin` não é necessário nem suportado nesta configuração.
