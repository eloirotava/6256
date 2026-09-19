# Agregação no envio (A-MPDU)

Este ramo (`ampdu-wip`) monta agregados no host. Ele **não** está pronto
para uso: o texto abaixo registra o que já se sabe do hardware e o que
falta responder, para que a investigação não recomece do zero.

## O que o chip exige

* O MAC não calcula o FCS das MPDUs de dentro de um agregado; o host
  precisa escrever os quatro bytes de verificação de cada uma.
* `MTX_AMPDU_CRC8_AUTO` atrapalha quando o agregado vem pronto do host:
  o CRC-8 do delimitador tem de ser escrito junto com ele.
* O comprimento total do agregado é obrigatório no descritor; sem ele o
  chip não transmite.
* O relatório de transmissão não serve para agregados: o chip devolveria
  o agregado inteiro ao host, o que é caro e nada diz sobre o que o outro
  lado recebeu. Quem resolve é o Block Ack.
* Uma MPDU sozinha dentro de um agregado é respondida com confirmação
  comum, não com Block Ack; por isso ela sai pelo caminho normal.

## O Block Ack Request

`ieee80211_send_bar()` copia o valor recebido para o quadro como ele vem,
e o campo é um controle de sequência: o número fica acima dos quatro bits
de fragmento. É preciso passar `IEEE80211_SN_TO_SEQ(sn)`.

Com o número cru, o outro lado lia uma janela dezesseis vezes atrás — o
driver pedia 732 e o ar mostrava "número 45, fragmento 12" — e o buffer
de reordenação dele nunca soltava o que vinha depois.

## O que já se confirmou no ar

Capturando de um ponto de acesso em modo monitor (`iw phy0 interface add
mon0 type monitor`, `tcpdump -i mon0 -s 160 -w ...`):

* Os agregados chegam ao outro lado e são desmontados por ele: as MPDUs
  aparecem com o campo de estado de A-MPDU no radiotap (cabeçalho de 64
  bytes em vez de 56) e com política de confirmação Block Ack (`0x0003`).
* Chegam decifradas — o AP aceitou o CCMP —, com FCS bom e números de
  sequência consecutivos, o que valida delimitador, CRC-8, alinhamento e
  o FCS escrito pelo host.
* Os BARs saem com o número certo depois da correção acima.

Duas leituras enganam nessas capturas e convém anotar:

* Os instantes das MPDUs de um mesmo agregado vêm quase iguais (deltas de
  poucos microssegundos) porque o receptor as carimba ao entregar o lote;
  isso não quer dizer que quadros não agregados estejam colados.
* O monitor de um AP não vê as respostas que o próprio rádio dele gera:
  não aparecem ACK, CTS nem Block Ack. A ausência deles na captura não
  prova nada.

## O que falta

O tráfego ainda não flui com agregação ligada, e nenhum Block Ack chega
ao driver (`ssv_agg_ba()` nunca é chamado com bitmap). Numa rajada de 200
pacotes o driver montou seis agregados e só quinze MPDUs agregadas foram
vistas no ar, o que sugere que a maior parte dos agregados não chega a
ser transmitida. O próximo passo é olhar o caminho entre `agg_build()` e
o chip: comprimento total, contagem de páginas pedidas ao chip e o que o
chip faz quando o pedido não cabe.
