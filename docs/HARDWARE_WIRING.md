# SEA STARS donanım bağlantı planı

Bu belge iki bilgiyi birbirinden ayırır:

- **Kurtarılan:** Yüklenen STM32 kodundan veya STM32 proje ayarından kesin çıkarılabilen bağlantı.
- **Yeni tasarım:** Arıza yalıtımı için IMU ve D300'e ayrılan bağımsız I2C yolları.
- **Canlı doğrulama:** Bağlı kartın çalışan register değerlerinden SWD ile doğrulanan bağlantı.

> v3.4 notu: D300 sürücüsü ve bağlantı bilgisi gelecekte tekrar kullanılabilmesi için korunur, ancak mevcut otonom görev D300 verisini kullanmaz. Dalış derinliği ölçülmez; balast yüzde/süre profili havuzda deneyle belirlenir.

Genel görsel şema: [hardware_wiring.svg](hardware_wiring.svg)

D300 ayrıntılı şeması ve referans notları:
[d300_stm32_wiring.svg](d300_stm32_wiring.svg) ·
[D300_STM32_INTEGRATION.md](D300_STM32_INTEGRATION.md)

## Canlı kart doğrulaması

1 Ağustos 2026'da bağlı NUCLEO-L152RE üzerinden SWD ile çalışan GPIO alternatif-fonksiyon ve timer register'ları okundu. `PA6/PA7` için AF2, `TIM3_CH1/CH2` için 50 Hz ve iki kanalda 1500 µs darbe görüldü. Böylece eski arşivdeki `PC7/PB6` kaydının bu karttaki güncel yazılıma ait olmadığı kesinleşti.

Yeni firmware projesi HAL/CMSIS, başlangıç kodu, interrupt katmanı ve derleme betiğiyle tamamlandı. Bununla birlikte fiziksel ESC sinyal fişlerinin gerçekten `PA6/D12` ve `PA7/D11` hatlarına gittiği enerji vermeden gözle/multimetreyle kontrol edilmelidir.

## STM32 sinyal tablosu

| İşlev | NUCLEO-L152RE pini | Durum | Karşı uç |
|---|---|---|---|
| Tank 1 STEP | `PA10` / D2 | Kurtarılan | Step sürücü 1 `STEP` |
| Tank 1 DIR | `PB3` / D3 | Kurtarılan | Step sürücü 1 `DIR` |
| Tank 2 STEP | `PB5` / D4 | Kurtarılan | Step sürücü 2 `STEP` |
| Tank 2 DIR | `PB4` / D5 | Kurtarılan | Step sürücü 2 `DIR` |
| Durum LED'i | `PA5` / D13 | Kurtarılan | Nucleo dahili LED |
| Kontrol istasyonu TX/RX | `PA2` / `PA3`, USART2 | Kurtarılan | Nucleo ST-LINK sanal COM |
| IMU I2C saat | `PB8` / D15, I2C1_SCL | Canlı doğrulama | IMU `C/SCL` |
| IMU I2C veri | `PB9` / D14, I2C1_SDA | Canlı doğrulama | IMU `D/SDA` |
| D300 I2C saat | `PB10`, I2C2_SCL | Yeni tasarım | D300 yeşil `SCL` |
| D300 I2C veri | `PB11`, I2C2_SDA | Yeni tasarım | Bu sensörde mavi `SDA` |
| ESC 1 PWM | `PA6` / D12, TIM3_CH1 | Canlı doğrulama | ESC 1 sinyal |
| ESC 2 PWM | `PA7` / D11, TIM3_CH2 | Canlı doğrulama | ESC 2 sinyal |

IMU ve D300 farklı donanımsal I2C çevre birimlerindedir. IMU `I2C1/PB8/PB9`, D300 ise `I2C2/PB10/PB11` kullanır. Böylece bir sensörün kablo, seviye dönüştürücü veya BUSY arızası diğer sensörün haberleşmesini durdurmaz. Firmware iki yolu da bağımsız sıfırlar ve yeniden dener.

## D300 bağlantısı

D300 kartı MS5837-30BA tabanlıdır ve firmware sürücüsü `0x76` I2C adresini kullanır.

| D300 hattı | Bağlantı |
|---|---|
| Besleme | Nucleo/harici regüle **5 V** |
| GND | Ortak sinyal toprağı |
| SCL | Yeşil kablo → `PB10 / CN10-25 / I2C2_SCL`, yalnızca 3.3 V lojik |
| SDA | Bu sensörde mavi kablo → `PB11 / CN10-18 / I2C2_SDA`, yalnızca 3.3 V lojik |

Üretici D300 kartı için 4.5–5.5 V besleme fakat 3.3 V I2C seviyesi belirtiyor ve 5 V I2C'nin karta zarar vereceğini özellikle söylüyor. Resmî kablo sırası kırmızı `5V`, siyah `GND`, yeşil `SCL`, sarı `SDA` şeklindedir; eldeki kabloda etiketli SDA mavidir. Renkten önce etikete güvenin. Modülde harici pull-up yoksa `PB10/SCL` ve `PB11/SDA` hatlarının her birinden **3V3'a 4.7 kΩ** ekleyin. Pull-up'ları 5 V'a bağlamayın ve modülde zaten varsa ikinci seti paralel eklemeyin.

## Dört pinli IMU soketi (`+ - C D`)

| Soket etiketi | NUCLEO-L152RE bağlantısı | İşlev |
|---|---|---|
| `+` | `3V3` | Güvenli sensör beslemesi |
| `-` | `GND` | Ortak sinyal toprağı |
| `C` | `PB8 / D15` | I2C1 `SCL` (clock) |
| `D` | `PB9 / D14` | I2C1 `SDA` (data) |

Bu, arkadaşınızın eski kodunda kullanılan dört telli haberleşme biçimidir. Kurtarılan kod `0x28` adresi ve `0xA0` chip ID'siyle BNO055 okuyordu; bu nedenle soketteki kartın BNO055 olma ihtimali vardır. Yeni `imu_i2c.c` hem BNO055'i hem de gerçek BNO085'i otomatik algılar ve seri hatta örneğin `SENSOR IMU:BNO055_I2C ADDR:0x28` yayınlar; tarayıcı bulunan modeli gösterir.

Gerçek Adafruit BNO085 kullanılıyorsa kart I2C modunda kalmalıdır: `P0/PS0=LOW`, `P1/PS1=LOW`. Yalnız dört pinli soket kullanıldığı için P0/P1'i haricen HIGH yapmayın. Çıplak sensör entegresine değil regülatörlü breakout karta bağlandığından emin olun; `+` için 3.3 V iki model açısından da güvenli seçimdir. Harici pull-up gerekiyorsa `C` ve `D` hatlarına yalnız 3.3 V'a 4.7 kΩ–10 kΩ eklenir; breakout üzerindeki pull-up'lar varsa ikinci set gerekmez.

## Step sürücüler ve balast motorları

Her sürücü için genel bağlantı şöyledir:

| Sürücü hattı | Bağlantı |
|---|---|
| `STEP` | Yukarıdaki STM32 STEP pini |
| `DIR` | Yukarıdaki STM32 DIR pini |
| Logic GND | STM32 ortak GND |
| `VMOT` ve motor GND | Motor üreticisinin gerilimine uygun sigortalı güç dağıtımı |
| A+/A-/B+/B- | Motor bobinleri; ohmmetreyle bobin çifti bulunarak sürücü datasheet'ine göre |
| `EN`, microstep pinleri, logic VDD | **Sürücü modeli öğrenildikten sonra** tamamlanacak |

Step sürücünün modeli, motor gerilimi/akımı, microstep ayarı ve limit switch bilgisi kodda yoktur. Bunlar olmadan akım limiti ve bobin pinleri çizilemez. Mekanik son noktada yalnız yazılımsal adım sayısına güvenmek yerine her tankın boş ve dolu ucuna limit switch eklenmesi önerilir.

## ESC ve fırçasız motorlar

Her ESC'nin sinyal ucu teyit edilen STM32 PWM pinine, sinyal GND'si ortak sinyal toprağına bağlanır. ESC'nin kalın güç kabloları sigortalı motor güç dağıtımına, üç faz çıkışı ilgili BLDC motora gider. Motor yönü yanlışsa önce yazılımdaki işaret/donanım eşlemesi doğrulanmalı; iki fazı değiştirmek ancak enerji tamamen kesikken yapılmalıdır.

ESC/BEC 5 V çıkışlarını birbirine veya USB'den beslenen Nucleo 5 V hattına doğrudan paralel bağlamayın. Tek bir seçilmiş, regüle mantık kaynağı kullanın. Motor akım dönüşlerini sensör toprağından geçirmenin IMU ve basınç ölçümünü bozmasını önlemek için yıldız topraklama ve ayrı motor/mantık dağıtımı kullanın.

## Güç ağacı

1. Batarya çıkışında ana sigorta ve erişilebilir fiziksel kill switch.
2. Sigortalı güç dağıtımından ESC1, ESC2 ve iki step sürücü VMOT hattı.
3. Ayrı regülatörden mantık 5 V: Nucleo ve D300.
4. Nucleo 3.3 V hattından dört pinli IMU ve I2C pull-up'ları.
5. Sinyal GND'leri tek yıldız noktasında birleşir; yüksek motor akımı bu ince dönüş hattından geçmez.

## Enerji vermeden önce cevaplanması gerekenler

1. İki balast tankı sağ/sol mu, ön/arka mı yerleşiyor?
2. Step sürücünün tam marka/modeli, motor akımı ve microstep ayarı nedir?
3. Boş/dolu limit switch var mı; yoksa eklenebilir mi?
4. ESC modeli, nötr/minimum/maksimum PWM değerleri ve motorların pozitif yönleri nedir?
5. IMU kartının üzerinde `BNO055` mi `BNO085/BNO086` mı yazıyor?

Bu bilgiler geldikten sonra güç, sürücü `EN`/microstep ve motor bobin bağlantıları üretim şemasına dönüştürülebilir.

## Kaynaklar

- [Degz Robotics D300 ürün bilgisi](https://degzrobotics.com/product/d300-depth-pressure-and-temperature-sensor/)
- [Degz Robotics D300 teknik wiki](https://wiki.degzrobotics.com/elektronik-kartlar/d300-derinlik-ve-su-sicakligi-sensoru/)
- [Blue Robotics MS5837 sürücüsü (MIT)](https://github.com/bluerobotics/BlueRobotics_MS5837_Library)
- [TE Connectivity MS5837-30BA veri sayfası](https://www.te.com/commerce/DocumentDelivery/DDEController?Action=srchrtrv&DocFormat=pdf&DocLang=English&DocNm=MS5837-30BA&DocType=Data+Sheet&PartCntxt=MS583730BA01-50)
- [Adafruit BNO085 pinleri ve haberleşme modu tablosu](https://learn.adafruit.com/adafruit-9-dof-orientation-imu-fusion-breakout-bno085/pinouts)
- [STM32L152RE datasheet](https://www.st.com/resource/en/datasheet/stm32l152re.pdf)
- [NUCLEO-L152RE kullanıcı kılavuzu](https://www.st.com/resource/en/user_manual/dm00105823.pdf)
