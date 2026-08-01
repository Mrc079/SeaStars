# SEA STARS Denizaltı Kontrol İstasyonu v3.4.1

Bu sürüm, NUCLEO-L152RE üzerindeki iki step balast tankını, iki fırçasız iticiyi ve BNO055/BNO085 IMU’yu yöneten modüler kontrol sistemidir. Eski tek dosyalı uygulama geri dönüş için `seastars_web-3 (1).py` adıyla korunmuştur.

## En önemli değişiklik

D300 görevden çıkarıldı. BNO055/BNO085 yalnız yönelim ölçer; **derinliği veya aracın gerçekten askıda kaldığını ölçemez**. Bu nedenle dalış artık havuzda deneyle bulunan açık çevrim bir profildir:

1. Manuel Kontrol sekmesinde iki tankı aynı yüzdeye göndererek aracı batıran değeri bulun.
2. Bir miktar battıktan sonra aracı yaklaşık askıda tutan yüzdeyi bulun.
3. Otonomi sekmesindeki `DALIŞ DEĞERİNE AL` / `ASKIDA DEĞERİNE AL` düğmeleriyle canlı iki-tank ortalamasını forma aktarın; süreleri girip kaydedin.
4. Önce düşük güçlü **Test rotası**, yalnız doğrulandıktan sonra **Görev rotası** kullanın.

Bu yaklaşım su yoğunluğu, yük, sızdırmazlık, batarya ve mekanik sürtünme değiştiğinde yeniden ayar gerektirir. Limit anahtarı ve derinlik geri beslemesi eklenmeden araç gözetimsiz kullanılmamalıdır.

## Simülatörde çalıştırma

```powershell
python run_station.py --sim
```

İlk deneme:

1. `SIMULATOR` portuna bağlanın.
2. `Donanım ayarları` içinde iki tank tam strokunu `1000` yapın.
3. Otonomi sekmesinde rotayı `Test`, otomatik başlatmayı `Açık`, süreyi örneğin `7 saniye` seçip profili kaydedin.
4. Canlı kartta `COUNTDOWN` görünür; sayaç boyunca iticiler etkinleşmez.
5. Süre dolunca `DIVE → HOVER_SETTLE → TEST_FORWARD → SURFACE` yürütülür.

Simülatör ve gerçek donanım ayrı ayar dosyaları kullanır.

## Gerçek STM32 ile çalıştırma

```powershell
python -m pip install -r requirements.txt
python run_station.py
```

Arayüz yalnız yerel bilgisayardan `http://127.0.0.1:5000` adresinde açılır. Ayarlar tarayıcıdan JSON olarak doğrulanır, atomik biçimde diske kaydedilir ve bağlantı kurulunca STM32’ye gönderilir.

## Manuel kullanım

- Ayrı bir ARM düğmesi yoktur. İlk sıfırdan farklı manuel itici komutu ESC’leri içeride etkinleştirir.
- İtici sürgüsü bırakılınca sıfıra döner.
- Sayısal panelde iskele ve sancak için bağımsız `-100…+100%` girilebilir.
- Tarayıcı odağı/heartbeat kaybolursa hem istasyon hem STM32 iticileri nötre alıp DISARM eder.
- `İTİCİLERİ DURDUR / DISARM` ve fiziksel kill switch korunur.
- Tank yüzde hedefi için tam strok kaydı ve o açılışta geçerli fiziksel boş sıfırı gerekir.

## Otomatik başlatma

Otonomi sekmesinde otomatik başlatma `Açık/Kapalı` seçilir. Açıkken bekleme süresi `1–86.400 saniye` arasında tam saniye olarak girilir. Sayaç, STM32 bağlantısı kurulup kayıtlı yapılandırma aktarıldıktan sonra hazırlanır.

- Sayaç sırasında step ve itici komutları kilitlidir; ESC’ler DISARMED kalır.
- Süre dolunca STM32 IMU ve tank kalibrasyonunu tekrar denetler, ESC’leri içten etkinleştirir ve rotayı başlatır.
- Normal bağlantı yenilemesi aynı MCU açılışında görevi ikinci kez başlatmaz.
- `SAYACI YENİDEN HAZIRLA` bilinçli yeni havuz denemesi içindir.
- Otomatik tank sıfırı varsayılan olarak kapalıdır. Yalnız kart açılırken iki tankın fiziksel olarak tamamen boş olduğu garanti edilebiliyorsa açılmalıdır; sistemde limit anahtarı yoktur.

### Test rotası

`DIVE → HOVER_SETTLE → TEST_FORWARD → SURFACE`

### Görev rotası

`DIVE → HOVER_SETTLE → 15+ sn düz → sağ 90° → 15+ sn düz → en az 360° daire → 15+ sn düz → sağ 90° → 15+ sn düz → SURFACE`

BNO055/BNO085 heading tutuşu ve seçilen roll/pitch ekseninde iki tank arasındaki diferansiyel dengeyi yürütür. İki tankla aynı anda hem roll hem pitch kapalı çevrim kontrol edilemez.

## Güvenlik davranışı

- Acil stop kilitlenir; açıkça kaldırılmadan hareket yapılamaz.
- Otonomi sırasında IMU verisi 600 ms’den eski olursa iticiler kapanır ve tank hedefleri %0 yapılır.
- Tank tam strok/sıfır bilgisi yoksa görev başlamaz.
- Dalış başlangıcında tanklar %0’a yakın değilse otomatik sayaç/görev reddedilir.
- Dönüş ve daire timeout’ları başarı sayılmaz; güvenli balast boşaltmaya geçilir.
- `AUTO ABORT` tankları boşaltır. `STOP` farklıdır: bütün hareketi anında tutar ve kilitler.
- İkili bir komutun ikinci yarısı gönderilemezse `STOP` uygulanır.

> Yazılım güvenliği fiziksel kill switch, sigorta, limit switch ve havuz başındaki operatörün yerini tutmaz.

## Testler ve firmware

```powershell
python -m unittest discover -s tests -v

cd STM32_otonomi\Firmware
powershell -ExecutionPolicy Bypass -File .\build_firmware.ps1
# Yalnız ESC/step ana gücü kapalı ve pervaneler sökülüyken:
powershell -ExecutionPolicy Bypass -File .\flash_firmware.ps1 -ConfirmMotorPowerOff
```

## Belgeler

- [Otonom algoritma ve deney sırası](docs/AUTONOMY.md)
- [Donanım bağlantı planı](docs/HARDWARE_WIRING.md)
- [Görsel devre şeması](docs/hardware_wiring.svg)
- [STM32 entegrasyon rehberi](docs/FIRMWARE_INTEGRATION.md)
- [D300 referans entegrasyonu — görevde kullanılmıyor](docs/D300_STM32_INTEGRATION.md)
- [D300 ayrıntılı bağlantı şeması — isteğe bağlı](docs/d300_stm32_wiring.svg)

## Klasör yapısı

- `seastars_station/`: web API, güvenlik denetleyicisi, protokol ve simülatör
- `seastars_station/static/`: Manuel Kontrol / Otonomi tarayıcı arayüzü
- `STM32_otonomi/Firmware/App/`: otonomi, IMU, D300 tanı ve runtime modülleri
- `STM32_otonomi/Firmware/Src/`: NUCLEO-L152RE HAL/platform kodu
- `STM32_otonomi/Firmware/Build/`: ELF/HEX/BIN ve derleme özeti
- `tests/`: donanımsız otomatik doğrulamalar
