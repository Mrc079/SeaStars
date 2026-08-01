# Eski sistemden güvenli geçiş

## Korunan STM32 protokolü

Yeni istasyon şu mevcut komutları değiştirmeden üretir: `S1/S2`, `V1/V2`, `ZERO1/ZERO2`, `T1/T2`, `ARM`, `DISARM`, `STOP`. Tank hareketi yine göreli adım, itici gücü yine `% × 2` ölçeğindedir. `POS1/POS2` ve `IMU H/R/P/C` telemetrileri kabul edilir; IMU için ondalıklı değerler de desteklenir.

## Bilinçli olarak değiştirilen davranışlar

- Eski sürüm hedef konumu komut gönderilmeden önce diske yazıyordu. Yeni sürüm yalnızca başarılı seri yazımından sonra hedefi değiştirir ve hareket hesabında güncel `POS` telemetrisini kullanır.
- Bağlantıda host konumunu `SETPOS` ile STM32'ye zorla yazma kaldırıldı. Konumun otoritesinin STM32 olması daha güvenlidir. Firmware bunu gerektiriyorsa protokol netleştirildikten sonra açık bir kurtarma akışı eklenmelidir.
- Ham komut konsolu varsayılan olarak kapalıdır.
- ARM, acil-stop ve heartbeat artık gerçek durum makineleridir; sadece ekrandaki rozet değildir.

## Önerilen doğrulama sırası

1. `python -m unittest discover -v`
2. `python run_station.py --sim` ile tüm kontrolleri deneme
3. Pervaneler ve balast aktüatörleri mekanik yükten ayrıyken gerçek STM32 bağlantısı
4. Sadece telemetri doğrulaması; motor komutu vermeme
5. Düşük hızda tek tek jog ve yön kontrolü
6. Fiziksel limit, acil stop ve kablo çıkarma testi
7. Kontrollü havuz testi

Eski dosya silinmediği için her aşamada geri dönüş mümkündür.

