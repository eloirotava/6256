# ssv6256: driver Linux para o Wi-Fi SSV6256 (SDIO)

Driver novo, para o mac80211, do chip iComm **SSV6256** (família "Turismo",
identificado como `SSV6006C`), usado em TV boxes. Substitui o driver do
fabricante (`ssv6x5x`), que está no branch `main` deste repositório.

**Em construção.** O que já funciona:

- transporte SDIO (portas de registrador e de dados);
- identificação do chip;
- inicialização da PLL, das tabelas de RF e de banda base;
- calibração de RF feita pelo host (DC de recepção, filtro RC em HT20 e
  HT40, vazamento de oscilador local e desequilíbrio IQ de TX e RX);
- inicialização do MAC (reset, relógio digital, tabela de registradores,
  contabilidade do buffer de pacotes, filtro de recepção);
- endereço MAC do e-fuse, do device tree (`local-mac-address`) ou
  aleatório, nessa ordem;
- carga do firmware `ssv6x5x-sw.bin` com verificação de checksum.

O que falta: caminho de dados (descritores de TX e RX), registro no
mac80211 e controle de taxa.

As calibrações levam cerca de 90 ms e são refeitas a cada carga do
módulo; o resultado aparece no `dmesg` em nível de depuração.

## Diferenças em relação ao SSV6051

| | SSV6051 | SSV6256 |
|---|---|---|
| Bandas | 2,4 GHz | 2,4 e 5 GHz |
| Largura | HT20 | HT20/40 |
| Status de envio | não informa por frame | o chip escreve o resultado no descritor |
| Agregação | montada pelo host | apoio do hardware, com bitmap |
| Calibração de RF | feita pelo firmware | feita pelo host |
| Barramento SDIO | igual | igual |

## Compilar e instalar

Precisa dos headers do kernel em execução:

```sh
sudo apt install build-essential linux-headers-$(uname -r)
make
sudo make install     # módulo em updates/, firmware em /lib/firmware/ssv/
```

Se o driver do fabricante (`ssv6x5x`) ou o `ssv6051` do Armbian estiverem
presentes, bloqueie-os em `/etc/modprobe.d/`, porque disputam o mesmo
dispositivo SDIO (3030:3030).
