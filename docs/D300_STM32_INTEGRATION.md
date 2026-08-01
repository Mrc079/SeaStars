# D300 — NUCLEO-L152RE entegrasyonu

Bu entegrasyon D300 üreticisinin önerdiği Blue Robotics MS5837 sürücüsünün
MIT lisanslı komut/hesaplama akışını STM32 HAL'a taşır. Tam, doğrulanabilir ve
lisanslı bir “D300 + STM32” örneği bulunamadığı için kaynağı belirsiz kod
arşivleri yerine D300 üretici örneği, sensör üreticisinin veri sayfası ve
Blue Robotics'in açık kaynak sürücüsü birlikte esas alındı.

Görsel bağlantı şeması: [d300_stm32_wiring.svg](d300_stm32_wiring.svg)

## Bu karttaki kesin bağlantı

| D300 kablosu | D300 işlevi | NUCLEO-L152RE | Not |
|---|---|---|---|
| Kırmızı | `VCC` | `+5V`, CN6 pin 5 | D300 kart girişi 4.5–5.5 V |
| Siyah | `GND` | `GND`, CN6 pin 6 | STM32 ile ortak toprak |
| Yeşil | `SCL` | `PB10`, CN10 pin 25 | I2C2 saat, 3.3 V lojik |
| Mavi | `SDA` | `PB11`, CN10 pin 18 | I2C2 veri, 3.3 V lojik |

Degz'in güncel dokümanındaki standart kabloda SDA sarıdır; eldeki sensörde
SDA mavi olarak etiketlenmiştir. Renge değil sensör üzerindeki `SDA/SCL`
etiketine güvenilmelidir.

## Pull-up ve besleme ayrıntısı

I2C hatları open-drain'dir. `SCL` ve `SDA` üzerinde zaten harici pull-up yoksa
her hatta birer **4.7 kΩ** direnç ile `3V3`'a pull-up eklenmelidir. Pull-up'lar
kesinlikle `5V`'a bağlanmamalıdır. Modülde hâlihazırda 4.7 kΩ civarında
pull-up varsa ikinci bir çift paralel eklenmemelidir.

D300 modülünün kırmızı besleme kablosu 5 V'a gider; bu, çıplak MS5837
entegresinin 1.5–3.6 V beslemesiyle karıştırılmamalıdır. D300 kartında gerilim
dönüştürücü bulunur. Uzun kabloda modülün girişine yakın `100 nF` seramik ve
`1–4.7 µF` bulk kondansatör eklemek besleme darbelerini azaltabilir.

## Firmware'deki referans akış

`STM32_otonomi/Firmware/App/Src/ms5837.c` şu diziyi uygular:

1. Sabit 7-bit adres `0x76` ile reset komutu `0x1E`, ardından 10 ms bekleme.
2. Yalnızca yedi PROM kelimesi: `0xA0, 0xA2, …, 0xAC`.
3. PROM CRC4 doğrulaması ve boş/bozuk katsayı kontrolü.
4. Basınç dönüşümü `D1/OSR8192 = 0x4A`, 20 ms bekleme ve 24-bit ADC okuma.
5. Sıcaklık dönüşümü `D2/OSR8192 = 0x5A`, 20 ms bekleme ve 24-bit ADC okuma.
6. MS5837-30BA birinci ve ikinci derece sıcaklık kompanzasyonu.
7. Yüzey basıncı, su yoğunluğu ve sensörün araç referansına montaj ofsetiyle
   metre cinsinden derinlik hesabı.

Önceki sürüm yanlışlıkla sekizinci `0xAE` PROM komutunu da gönderiyordu.
TE veri sayfası ve Blue Robotics referansı PROM'un `0xAC`'de bittiğini
doğruladığından bu işlem kaldırıldı. Sürücü artık reset, PROM komutu, PROM
okuma, CRC, ADC komutu ve ADC okuma hatalarını ayrı ayrı raporlar.

## Mevcut fiziksel teşhis

Bağlı karttaki son canlı taramada `PB10/PB11` iki hat da HIGH ve I2C yolu boş
olmasına rağmen `0x76` adresinden ACK alınamadı. HAL hata bayrağı `0x04`, yani
ACK failure'dır. Bu durum PROM hesabından önce, adresleme aşamasında oluşur;
dolayısıyla yazılım düzeltmesi tek başına mevcut fiziksel `NO_ACK` durumunu
gideremez.

Enerji açıkken multimetreyle şu üç ölçüm yapılmalıdır:

1. D300 kırmızı–siyah arasında yaklaşık 5 V.
2. D300 yeşil–siyah arasında boşta yaklaşık 3.3 V.
3. D300 mavi–siyah arasında boşta yaklaşık 3.3 V.

Kırmızı–siyah 5 V olup iki veri hattından biri 0 V ise kısa devre/yanlış kablo;
ikisi 5 V ise tehlikeli 5 V pull-up; ikisi 3.3 V olup ACK yoksa konnektör sırası,
kablo sürekliliği veya D300 kart arızası kontrol edilmelidir.

## Kaynaklar

- [Degz D300 teknik sayfası](https://wiki.degzrobotics.com/tr/elektronik-kartlar/d300-derinlik-ve-su-sicakligi-sensoru/)
- [Degz D300 kullanım örneği](https://wiki.degzrobotics.com/tr/elektronik-kartlar/d300-derinlik-ve-su-sicakligi-sensoru/usage/)
- [Blue Robotics MS5837 kaynak kodu](https://github.com/bluerobotics/BlueRobotics_MS5837_Library/blob/master/src/MS5837.cpp)
- [Blue Robotics MIT lisansı](https://github.com/bluerobotics/BlueRobotics_MS5837_Library/blob/master/LICENSE)
- [TE Connectivity MS5837-30BA Rev C6 veri sayfası](https://www.te.com/commerce/DocumentDelivery/DDEController?Action=srchrtrv&DocFormat=pdf&DocLang=English&DocNm=MS5837-30BA&DocType=Data+Sheet&PartCntxt=MS583730BA01-50)
