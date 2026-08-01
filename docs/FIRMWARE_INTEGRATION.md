# STM32 firmware entegrasyon rehberi

## Mevcut durum

Yeni sensör ve otonomi kodu `STM32_otonomi/Firmware/App` altında bağımsız modüller olarak hazırlandı:

- `ms5837.c`: D300/MS5837-30BA reset, PROM CRC4, 24 bit ölçüm, birinci/ikinci derece sıcaklık-basınç kompanzasyonu, yüzey basıncı ve montaj ofseti.
- `imu_i2c.c`: yalıtılmış I2C1 hattında BNO085 SH-2/SHTP ve eski BNO055 otomatik algılama, quaternion/Euler dönüşümü ve kalibrasyon telemetrisi.
- `Middleware/Adafruit_SH2`: BNO085 için lisansı ve NOTICE dosyasıyla birlikte resmi SH-2/SHTP C katmanı.
- `autonomy.c`: IMU geri beslemeli ve açık çevrim balast profilli, non-blocking test/görev durum makinesi.
- `seastars_runtime.c`: web istasyonunun seri protokolü, manuel hareket, güvenlik kilitleri ve telemetri.

Firmware artık NUCLEO-L152RE için kendi HAL/CMSIS sürücüleri, başlangıç ve linker dosyaları, GPIO/periferik katmanı ve güvenli ana döngüsüyle eksiksizdir. `build_firmware.ps1` ARM GCC ile doğrulanmış `.elf`, `.hex` ve `.bin` üretir. Eski uygulama yalnız geri dönüş ve pin karşılaştırması için [legacy_main_2026-07-31.c](../STM32_otonomi/Recovered/legacy_main_2026-07-31.c) altında korunur.

## CubeMX'te oluşturulacak periferikler

| Periferik | Ayar | Amaç |
|---|---|---|
| USART2 | PA2 TX, PA3 RX, 115200 8N1, RX interrupt | PC web istasyonu/ST-LINK VCP |
| I2C1 | PB8 SCL, PB9 SDA, 100 kHz | Yalnız IMU: BNO055 `0x28/0x29`; BNO085 `0x4A/0x4B`; SCL/STOP bus recovery |
| I2C2 | PB10 SCL, PB11 SDA, 10 kHz | Yalnız D300/MS5837-30BA `0x76`; uzun kablo toleransı, SCL/STOP bus recovery, açılışta ACK ve adres taraması tanısı |
| TIM2 | update interrupt | Tank 1 step pulse üretimi |
| TIM5 | update interrupt | Tank 2 step pulse üretimi |
| TIM3 | PA6/CH1 ve PA7/CH2, 50 Hz, açılışta 1500 µs | ESC sinyalleri; canlı karttan doğrulandı |
| GPIO output | PA10, PB3, PB5, PB4 | STEP1, DIR1, STEP2, DIR2 |
| SYS | Serial Wire | SWD debug |
| IWDG | Donanım testlerinden sonra etkin | Ana döngü kilitlenmesine karşı son savunma |

`PA6/PA7` seçimi bağlı kartın çalışan register'larından SWD ile doğrulandı. Eski sürümün `PC7/PB6` kaydı bu donanım revizyonunda kullanılmıyor.

## Derleme

```powershell
cd STM32_otonomi\Firmware
powershell -ExecutionPolicy Bypass -File .\build_firmware.ps1
```

Çıktılar `Firmware/Build` klasörüne yazılır. Dört telli çift-model IMU sürücülü doğrulanmış derlemede flash kullanımı yaklaşık %10,4, RAM kullanımı yaklaşık %11,1'dir. Karta yazmadan önce `Backups` klasöründeki orijinal flash yedeğini koruyun; ilk çıkış testi motor ana gücü kapalıyken osiloskop/logic analyzer ile yapılmalıdır.

## Çalışma zamanlaması

- IMU: I2C üzerinden BNO085 için 20 ms rotation-vector raporu / 5 ms servis; BNO055 için 50 ms Euler okuması. 1,5 saniye veri kesilirse sürücü kayıp sayılır ve otomatik yeniden aranır.
- D300: 100 ms'de bir örnek; OSR8192 basınç+sıcaklık dönüşümü yaklaşık 40 ms sürer.
- Kontrol döngüsü: 20 ms hedef periyot.
- Telemetri: 250 ms.
- D300 yüzey sıfırı: sekiz örnek ortalaması; yalnız araç sabit ve sensör yüzeydeyken yapılır.

Yüzey sıfırı sırasında D300 okuması kısa süre bloklayabilir. IWDG kullanılırsa timeout bu işlemi kapsayacak kadar uzun seçilmeli ve watchdog yalnız sağlıklı ana döngü noktasından beslenmelidir.

## Seri protokol

İstasyonun kullandığı temel komutlar:

| Komut | İşlev |
|---|---|
| `STOP` | Acil stop kilidi; ESC sıfır, step hedefi mevcut konumda tutulur |
| `HELLO` / `PING` | READY el sıkışması / manuel sürüş bağlantı watchdog'u |
| `DISARM` / `ARM` | İç itici güvenlik durumu; web’de ayrı ARM düğmesi yoktur |
| `T1 n`, `T2 n` | -200…200 eski ölçek; runtime içinde yüzdeye çevrilir |
| `S1 n`, `S2 n` | Göreli step hedefi |
| `V1 n`, `V2 n` | 50…2000 step/s |
| `ZERO1`, `ZERO2` | Operatör onaylı boş konum sıfırı |
| `CFG ad değer` | Tarayıcıdaki kalibrasyon/görev ayarı |
| `DEPTH_ZERO` | D300 yüzey basıncı ortalaması |
| `AUTO SCHEDULE` | Açık durumdaki kayıtlı saniye sayacını MCU açılışı başına bir kez hazırlar |
| `AUTO PREPARE` | Operatörün bilinçli yeni denemesi için sayacı yeniden hazırlar |
| `AUTO START` / `AUTO ABORT` | Eski/servis amaçlı anlık başlatma / balastı boşaltma |
| `TURN -90`, `TURN 90`, `TURN 180` | Bulunan IMU ile kapalı çevrim manuel dönüş |

Otonom görev sırasında ayar ve manuel hareket komutları gömülü tarafta da reddedilir. `STOP`, görevi sonlandırır ve balastları olduğu yerde tutar; `AUTO ABORT` ise balastları boşaltarak yüzeye çıkma komutudur.

İlk sıfırdan farklı manuel itici komutundan önce istasyon iç `ARM` komutunu otomatik yollar. Manuel sürüşte 1,5 saniye host teması gelmezse STM32 bağımsız olarak ESC'leri nötre alıp DISARM olur. Otonom görev host bağlantısından bağımsız devam eder; IMU veya tank kalibrasyon hatasında balast boşaltma durumuna geçer. D300 görevi kilitlemez ve maksimum derinlik denetimi yapmaz.

## Donanım üstü kabul testleri

1. D300 bağlı değilken test rotası IMU ve tank koşulları hazırsa başlayabiliyor mu?
2. `COUNTDOWN` boyunca PWM’ler nötr ve tank hedefleri hareketsiz mi?
3. IMU `C` veya `D` kablosu çıkarıldığında görev `FAULT_SURFACE` durumuna geçip ESC’leri durduruyor ve tankları boşaltıyor mu?
4. `STOP` basıldığında iki PWM nötre geliyor ve iki step timer duruyor mu?
5. Sağ 90° komutu araç yönüne göre gerçekten sağa mı dönüyor? Değilse heading veya motor işareti düzeltilmeli.
6. Her düz etap ölçülen süre olarak en az 15 saniye mi?
7. Yüzeye çıkış timeout’u başarı gibi raporlanmıyor, `SURFACE_TIMEOUT` hatası veriyor mu?

Bu testler küvet/havuz testinden önce askıda, pervanesiz ve akım sınırlı güç kaynağıyla tamamlanmalıdır.
