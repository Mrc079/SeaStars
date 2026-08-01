# Otonom sürüş tasarımı — açık çevrim balast

## Ölçüm sınırı

BNO055/BNO085; yaw/heading, roll ve pitch verir. Mutlak derinlik, düşey hız veya aracın askıda kalıp kalmadığını vermez. D300 iptal edildiği için yazılım `30 cm` derinliği ölçerek doğrulayamaz. Dalış; deneyle bulunan tank doluluk yüzdesi ve süre üzerinden yürütülür.

Bu nedenle arayüzdeki değerlerin anlamı şöyledir:

| Ayar | Anlamı |
|---|---|
| Dalış balastı | Dalış evresinde iki tankın ortak temel doluluğu, tam strok yüzdesi |
| Dalışta bekleme | Bu hedef uygulandıktan sonra askıda profile geçmeden beklenecek süre |
| Askıda balast | Yatay rota boyunca kullanılacak ortak temel doluluk |
| Askıda dengeleme | İticiler başlamadan önce tankların yeni hedefe yaklaşması için bekleme |

Seçilen `roll` veya `pitch` eksenindeki IMU açısı temel yüzdeden bir tanka eklenip diğerinden çıkarılır. Düzeltme `Maksimum tank farkı` ile sınırlandırılır.

## Otomatik başlatma durumları

1. Kontrol istasyonu kayıtlı ayarları STM32’ye yollar.
2. Otomatik başlatma `Açık` ise `AUTO SCHEDULE`, girilen saniye değeriyle kart açılışı başına yalnız bir kez kabul edilir.
3. `COUNTDOWN` boyunca ESC nötr/DISARMED ve manuel hareket kilitlidir.
4. Sayaç sonunda IMU tazeliği, iki tam strok, geçerli sıfırlar ve tankların yaklaşık %0 konumu yeniden kontrol edilir.
5. Koşullar uygunsa iç ARM uygulanır ve `DIVE` başlar; değilse `FAULT_SURFACE` ile tanklar %0 hedefe gider.

`AUTO PREPARE`, operatörün web arayüzündeki “Sayacı yeniden hazırla” işlemi için bilinçli yeniden kurmadır. Seri bağlantının gidip gelmesi normal `AUTO SCHEDULE` üzerinden görevi tekrarlamaz.

## Test rotası

1. `DIVE`: Dalış balast yüzdesi uygulanır, iticiler sıfırdır.
2. `HOVER_SETTLE`: Askıda balast yüzdesi ve IMU diferansiyel düzeltmesi uygulanır.
3. `TEST_FORWARD`: Düşük test gücüyle heading tutulur.
4. `SURFACE`: ESC DISARM, iki tank hedefi %0.
5. Tanklar yaklaşık %0’a geldiğinde `COMPLETE`.

Test rotası ilk su testlerinde ileri gücü `0–15%`, süreyi kısa tutmak için tasarlanmıştır.

## Görev rotası

Firmware bloklayıcı beklemeler yerine yaklaşık her 20 ms’de ilerleyen bir durum makinesi kullanır:

1. `DIVE`
2. `HOVER_SETTLE`
3. `STRAIGHT_1`: en az 15 saniye heading tutuşu
4. `TURN_1`: başlangıca göre +90° sağ dönüş
5. `STRAIGHT_2`: en az 15 saniye
6. `CIRCLE`: farklı itici güçleri; IMU heading değişimi toplam en az 360°
7. `STRAIGHT_3`: en az 15 saniye
8. `TURN_2`: +90° sağ dönüş
9. `STRAIGHT_4`: en az 15 saniye
10. `SURFACE`

Düz gidişte:

```text
heading_hatası = hedef_heading - ölçülen_heading
sol  = ileri_güç + Kp × heading_hatası
sağ  = ileri_güç - Kp × heading_hatası
```

Dönüşte iki itici zıt yönde sürülür. Hedef açı çevresinde 800 ms kararlı kalmadan bir sonraki etaba geçilmez.

## 1 metre daire sınırı

IMU yalnız yön değişimini doğrular. X/Y konumu veya doğrusal hızı ölçmediğinden dairenin gerçekten 1 metre çapında olduğunu kanıtlayamaz. `Daire ileri gücü` ve `Daire dönüş farkı`, zeminde/havuz kenarında işaretlenmiş en az 1 metre çap üzerinde deneyle ayarlanmalıdır. Akıntı ve batarya geriliminden bağımsız garanti için DVL, akustik konumlama veya dış kamera gerekir.

## Yanal hareket sınırı

İki paralel yatay itici ileri/geri, kavis ve olduğu yerde yaw dönüşü sağlar. Gerçek sağa/sola öteleme yapmaz. Strafe için yana bakan üçüncü itici veya uygun açılı dört yatay itici gerekir.

## Hata davranışı

Aşağıdakilerde iticiler kapanır ve tank hedefleri %0 yapılır:

- IMU verisinin 600 ms’den eski olması,
- tank tam strok/sıfır bilgisinin kaybolması,
- dönüş veya daire timeout’u,
- yüzeye çıkışta tankların süre içinde boşalamaması,
- geçersiz rota ayarı.

D300 yokluğu ve ölçülen derinlik artık görev hatası değildir. Bunun sonucu olarak yazılım aşırı derinliği de algılayamaz; mekanik/elektrik güvenlik ve bağlı düşük güçlü test zorunludur.

## Balast değeri bulma sırası

1. Pervaneler enerjisizken tankların `DIR` yönünü doğrulayın.
2. Tanklar tamamen boşken iki ekseni sıfırlayın.
3. Tam dolu mekanik noktayı düşük hızda bulun, adımları kaydedin; mümkünse boş/dolu limit switch ekleyin.
4. Aracı emniyet halatıyla suya alın; otomatik başlatma `Kapalı` olsun.
5. İki tanka düşük yüzdelerle birlikte komut verin. Aracı yavaşça batıran en küçük değeri not edin.
6. Bir miktar battıktan sonra yaklaşık sabit seviyede kaldığı değeri not edin.
7. Bu iki değeri Otonomi sekmesine girin; test ileri gücünü önce `0%` yaparak yalnız dalış/boşaltmayı deneyin.
8. Sonraki testte `5–15%` kısa ileri test uygulayın.
9. Heading Kp ve 90° dönüş yönünü doğrulayın.
10. İşaretli 1 metre çapta daire güçlerini ayarlayın; en son tam görev rotasını çalıştırın.

Her aşamada fiziksel kill switch başında ayrı bir operatör bulunmalıdır.
