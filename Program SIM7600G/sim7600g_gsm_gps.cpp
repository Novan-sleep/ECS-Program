/*
 * sim7600g_gsm_gps.cpp
 * ESP32 DevKit V4 - Modul SIM7600G-H (4G LTE + GPS)
 *
 * Tujuan: inisialisasi modem 4G LTE, cek status jaringan, lalu
 * mengaktifkan dan membaca data GPS (GNSS) dari modul SIM7600G.
 *
 * Hardware:
 *  - ESP32 DevKit V4
 *  - SIM7600G-H via UART1
 *      ESP32 RX1 (GPIO26) <- SIM7600 TXD
 *      ESP32 TX1 (GPIO27) -> SIM7600 RXD
 *      ESP32 GPIO25       -> SIM7600 PWRKEY (tekan sebentar untuk power on)
 *  - Catu daya SIM7600G terpisah (5V/2A min), GND digabung dengan ESP32
 *
 * Pendekatan: AT command manual (tanpa library TinyGSM) agar mudah
 * dikustomisasi dan didebug lewat Serial Monitor.
 *
 * ============================================================
 * TROUBLESHOOTING - LED NET (kuning) berkedip cepat terus-menerus
 * ============================================================
 * Arti indikator LED NET pada modul SIMCOM SIM7600 (umum untuk board
 * breakout SIM7600G-H/E-H dsb):
 *   - Mati total           : modul tidak menyala / tidak ada power
 *   - Berkedip cepat        : modul ON tapi BELUM registrasi ke jaringan
 *     terus-menerus           (sedang searching operator/network)
 *     (± 100-300ms interval)
 *   - Berkedip lambat       : sudah registrasi ke jaringan, idle
 *     (64ms ON, 800ms OFF)
 *   - Berkedip sangat lambat: sudah registrasi & PPP/data terhubung
 *     (64ms ON, 3000ms OFF)
 *   - Menyala terus         : sedang melakukan panggilan suara
 *
 * Jadi "kuning mati-nyala cepat" = modul hidup normal, tapi GAGAL
 * registrasi ke jaringan seluler. Urutan pengecekan yang disarankan:
 *
 * 1. Kartu SIM
 *    - Pastikan SIM card terpasang dengan benar, kontak bersih, dan
 *      posisi/orientasi sesuai slot (chip menghadap ke bawah/atas
 *      sesuai cetakan holder).
 *    - Pastikan SIM aktif/berlaku (bisa dites di HP biasa dulu).
 *    - Cek AT+CPIN? harus balas "+CPIN: READY". Jika "SIM PIN"/"SIM PUK"
 *      berarti kartu terkunci PIN -> perlu AT+CPIN="1234" dulu.
 *    - Cek AT+CCID untuk memastikan modul benar-benar membaca ICCID SIM
 *      (kalau kosong/error -> SIM tidak terbaca sama sekali, cek fisik).
 *
 * 2. Antena
 *    - Pastikan antena 4G/LTE (bukan antena GPS) terpasang di konektor
 *      MAIN/ANT yang benar dan terpasang kencang (konektor IPEX/SMA
 *      sering kendor/salah pasang).
 *    - Coba pindah lokasi/dekat jendela, hindari area tertutup logam
 *      atau ruangan basement dengan sinyal lemah.
 *
 * 3. Catu daya (paling sering jadi penyebab!)
 *    - SIM7600 butuh arus burst hingga ~2A saat transmit/searching
 *      network. Power supply dari USB ESP32 atau regulator lemah akan
 *      menyebabkan brownout modul saat mencoba registrasi -> gagal
 *      terus, LED tetap kedip cepat / modul restart sendiri.
 *    - Gunakan catu daya terpisah 5V/2A (bukan dari 5V pin ESP32 dev
 *      board), dengan kapasitor besar (>1000uF) dekat modul jika perlu.
 *    - GND catu daya modul HARUS digabung dengan GND ESP32 (common
 *      ground), meskipun sumber tegangan terpisah.
 *
 * 4. Konfigurasi jaringan/operator
 *    - Cek AT+CSQ, nilai RSSI (angka pertama) idealnya >10 (skala 0-31,
 *      99 = tidak terbaca sama sekali -> masalah antena/sinyal).
 *    - Cek AT+COPS? , jika kosong (+COPS: 0) berarti belum ketemu
 *      operator -> coba paksa scan manual: AT+COPS=?  (butuh waktu lama,
 *      list operator yang terdeteksi di sekitar).
 *    - Cek AT+CEREG? / AT+CREG? , status "0,2" = sedang searching terus,
 *      "0,1" atau "0,5" = sudah registrasi (home/roaming).
 *    - Cek mode jaringan AT+CNMP? , coba paksa ke Auto: AT+CNMP=2
 *      (2=Auto, 13=GSM only, 38=LTE only, 51=GSM+LTE, dst).
 *    - Beberapa operator/kartu IoT butuh APN manual: AT+CGDCONT=1,"IP","apn-operator"
 *
 * 5. Firmware/hardware modul
 *    - Cek AT+CGMR untuk versi firmware, kadang firmware lama punya bug
 *      registrasi -> update firmware via tool SIMCOM jika perlu.
 *    - Coba restart modul: AT+CFUN=1,1 (full functionality + reset)
 *      atau power cycle total (lepas power beberapa detik).
 *    - Jika modul sama sekali tidak balas "AT" -> OK, cek wiring TX/RX
 *      (kemungkinan tertukar) dan level tegangan UART (SIM7600 modul
 *      breakout umumnya sudah 3.3V/5V tolerant, cek datasheet board-nya).
 *
 * Ringkasan cepat: kalau LED kedip cepat terus tanpa pernah melambat,
 * 90% kasus di lapangan adalah SIM tidak terbaca/terkunci PIN, antena
 * lepas/salah slot, atau power supply tidak cukup kuat saat modul
 * mencoba transmit ke BTS.
 *
 * Gunakan fungsi runDiagnostics() di bawah untuk menjalankan semua
 * pengecekan AT command di atas sekaligus dan cetak hasil + interpretasi
 * ke Serial Monitor.
 * ============================================================
 */

#include <Arduino.h>

#define MODEM_RX_PIN     26
#define MODEM_TX_PIN     27
#define MODEM_PWRKEY_PIN 25
#define MODEM_BAUD       115200

HardwareSerial ModemSerial(1);

String sendAT(const String &cmd, unsigned long timeout = 2000) {
  ModemSerial.println(cmd);
  String response = "";
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (ModemSerial.available()) {
      response += (char)ModemSerial.read();
    }
  }
  response.trim();
  return response;
}

void powerOnModem() {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(3000); // beri waktu modul boot
}

bool waitModemReady() {
  for (int i = 0; i < 10; i++) {
    String resp = sendAT("AT");
    if (resp.indexOf("OK") >= 0) return true;
    delay(1000);
  }
  return false;
}

void checkNetworkStatus() {
  Serial.println("-- Cek Status Jaringan 4G LTE --");
  Serial.println(sendAT("AT+CPIN?"));      // status SIM card
  Serial.println(sendAT("AT+CSQ"));        // kekuatan sinyal
  Serial.println(sendAT("AT+COPS?"));      // operator terdaftar
  Serial.println(sendAT("AT+CEREG?"));     // status registrasi LTE
  Serial.println(sendAT("AT+CNMP?"));      // mode jaringan aktif
}

// Jalankan semua pengecekan diagnostik untuk kasus "LED NET kedip cepat
// terus-menerus" (modul gagal registrasi jaringan). Lihat catatan
// TROUBLESHOOTING di bagian atas file untuk arti tiap hasil.
void runDiagnostics() {
  Serial.println();
  Serial.println("======== DIAGNOSTIK SIM7600G-H ========");

  Serial.println("[1] Cek modul merespon AT ...");
  Serial.println(sendAT("AT"));

  Serial.println("[2] Info modul (model & firmware) ...");
  Serial.println(sendAT("ATI"));         // model modul
  Serial.println(sendAT("AT+CGMR"));     // versi firmware

  Serial.println("[3] Cek SIM card ...");
  String cpin = sendAT("AT+CPIN?");
  Serial.println(cpin);
  if (cpin.indexOf("READY") >= 0) {
    Serial.println("    -> SIM terbaca & tidak terkunci. OK.");
  } else if (cpin.indexOf("SIM PIN") >= 0) {
    Serial.println("    -> SIM terkunci PIN! Kirim AT+CPIN=\"kode_pin\" dulu.");
  } else {
    Serial.println("    -> SIM TIDAK TERBACA. Cek pemasangan fisik SIM card.");
  }
  Serial.println(sendAT("AT+CCID")); // ICCID, harus muncul nomor jika SIM kebaca

  Serial.println("[4] Kekuatan sinyal (RSSI) ...");
  String csq = sendAT("AT+CSQ");
  Serial.println(csq);
  Serial.println("    -> Format: +CSQ: <rssi>,<ber>. rssi 0-31 (makin besar makin");
  Serial.println("       bagus), 99 = TIDAK ADA SINYAL SAMA SEKALI (cek antena!).");

  Serial.println("[5] Status registrasi jaringan ...");
  Serial.println(sendAT("AT+CREG?"));   // registrasi 2G/3G
  Serial.println(sendAT("AT+CEREG?"));  // registrasi LTE
  Serial.println("    -> Format: +C(E)REG: <n>,<stat>. stat=1 atau 5 = registrasi");
  Serial.println("       OK (home/roaming). stat=0 atau 2 = belum/sedang searching.");

  Serial.println("[6] Operator & mode jaringan ...");
  Serial.println(sendAT("AT+COPS?"));   // operator terdaftar saat ini
  Serial.println(sendAT("AT+CNMP?"));   // mode jaringan (auto/gsm/lte)

  Serial.println("[7] Catu daya - cek modul tidak restart sendiri ...");
  Serial.println(sendAT("AT+CBC"));     // status baterai/power (jika didukung board)

  Serial.println("========================================");
  Serial.println("Jika RSSI=99 atau CREG/CEREG stat tetap 0/2 setelah beberapa");
  Serial.println("kali dicoba: cek antena & power supply sesuai catatan di atas");
  Serial.println("file ini (bagian TROUBLESHOOTING).");
  Serial.println();
}

// Registrasi jaringan (CEREG/CREG "OK") BERBEDA dengan sudah punya koneksi
// internet. Modul bisa saja sudah registrasi ke BTS tapi APN/PDP context
// data belum aktif, sehingga tetap tidak bisa akses internet. Fungsi ini
// mengecek status koneksi data yang sebenarnya, sampai tes HTTP GET nyata.
//
// Ganti APN "internet" sesuai kartu SIM/operator yang dipakai (misal
// Telkomsel: "internet", XL: "internet", operator IoT: APN khusus dari
// provider-nya).
// Terjemahkan <statuscode> dari +HTTPACTION sesuai manual resmi SIMCOM
// "SIM7500_SIM7600_SIM7800 Series_HTTP_AT Command Manual" bab 4 & 5.
// Catatan: 100-505 & 600-604 adalah kode dari tabel bab 4 (HTTP response +
// beberapa kode koneksi umum), sedangkan 0 & 701-719 dari tabel bab 5
// (kode error internal modul). Modul memakai KEDUA tabel ini untuk field
// <statuscode> yang sama, jadi harus dicek gabungan seperti di bawah.
String interpretHttpStatus(int code) {
  switch (code) {
    case 200: return "OK - request berhasil, internet AKTIF & terpakai";
    case 301: case 302: case 303: case 307:
      return "Redirect (server minta pindah URL, misal http->https). Koneksi internet sebenarnya JALAN.";
    case 400: return "Bad Request (URL/parameter salah dari sisi kita)";
    case 403: return "Forbidden (server menolak akses)";
    case 404: return "Not Found (URL tidak ada di server, tapi internet JALAN)";
    case 500: case 502: case 503: case 504:
      return "Error di sisi server tujuan (bukan masalah modul/internet kita)";
    case 600: return "Not HTTP PDU (respons bukan format HTTP valid)";
    case 601: return "Network Error - masalah jaringan data";
    case 602: return "No memory di modul";
    case 603: return "DNS Error - gagal resolve domain (nama server tidak ditemukan)";
    case 604: return "Stack Busy - coba ulang beberapa saat lagi";
    case 0:   return "Success (kode 0 dari tabel error internal)";
    case 701: return "Alert state";
    case 702: return "Unknown error - error generik modul, biasanya karena PDP context/APN belum benar-benar siap saat HTTPACTION dijalankan, atau sinyal terlalu lemah/putus saat request. Coba lagi setelah pastikan AT+CGPADDR sudah punya IP dan AT+CSQ sinyal cukup (<20).";
    case 703: return "Busy - sesi HTTP sebelumnya belum ditutup, pastikan AT+HTTPTERM dipanggil sebelum request baru";
    case 704: return "Connection closed error - koneksi terputus di tengah request";
    case 705: return "Timeout - server tidak merespons dalam waktu yang ditentukan";
    case 706: return "Gagal kirim/terima data socket";
    case 707: return "File tidak ada / error memori lain";
    case 708: return "Parameter tidak valid (cek AT+HTTPPARA yang dikirim)";
    case 709: return "Network error (level socket)";
    case 712: return "Gagal membuat socket";
    case 713: return "Get DNS failed - domain tidak bisa di-resolve (cek APN & signal)";
    case 714: return "Gagal connect socket ke server";
    case 715: return "Handshake gagal (khusus HTTPS/SSL)";
    case 717: return "No network error - tidak ada jaringan data sama sekali";
    case 718: return "Send data timeout";
    default:  return "Kode tidak dikenal, cek manual SIMCOM HTTP AT Command bab 4 & 5";
  }
}

// Coba GET ke satu URL, return <statuscode> dari +HTTPACTION (-1 kalau
// tidak dapat respons sama sekali/timeout total).
int doHttpGet(const char *url) {
  sendAT("AT+HTTPTERM"); // pastikan sesi HTTP lama tertutup dulu
  Serial.println(sendAT("AT+HTTPINIT", 10000));
  Serial.println(sendAT("AT+HTTPPARA=\"CID\",1"));
  String setUrl = "AT+HTTPPARA=\"URL\",\"" + String(url) + "\"";
  Serial.println(sendAT(setUrl));

  String httpAction = sendAT("AT+HTTPACTION=0", 15000); // 0 = GET
  Serial.println(httpAction);
  // Modul kirim URC terpisah: +HTTPACTION: 0,<statuscode>,<datalen>. Kalau
  // belum tertangkap di respons pertama, tunggu sebentar lagi.
  String httpUrc = httpAction;
  if (httpUrc.indexOf("+HTTPACTION:") < 0) {
    delay(3000);
    httpUrc = sendAT("", 3000);
    Serial.println(httpUrc);
  }
  sendAT("AT+HTTPTERM");

  int actionPos = httpUrc.indexOf("+HTTPACTION:");
  if (actionPos < 0) return -1;
  int firstComma = httpUrc.indexOf(',', actionPos);
  int secondComma = httpUrc.indexOf(',', firstComma + 1);
  if (firstComma < 0 || secondComma < 0) return -1;
  return httpUrc.substring(firstComma + 1, secondComma).toInt();
}

bool checkInternetConnection(const char *apn = "internet") {
  Serial.println();
  Serial.println("-- Cek Koneksi Internet (Data/PDP Context) --");

  // Set APN pada PDP context 1. PDP context akan diaktifkan otomatis oleh
  // AT+HTTPINIT nanti (sesuai alur resmi SIMCOM) - TIDAK PERLU AT+CGACT
  // manual di sini, memanggil keduanya bisa bentrok status PDP context.
  String setApn = "AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\"";
  Serial.println(sendAT(setApn));

  // Cek IP address yang didapat modul dari operator
  String ipResp = sendAT("AT+CGPADDR=1");
  Serial.println(ipResp);
  // Format balasan +CGPADDR bisa dengan atau TANPA tanda kutip tergantung
  // firmware modul, contoh: +CGPADDR: 1,10.112.57.121  atau  +CGPADDR: 1,"10.112.57.121"
  // Jadi cukup cari prefix "+CGPADDR: 1," lalu pastikan setelahnya bukan
  // langsung koma/newline (yang berarti IP kosong / gagal dapat alamat).
  int ipPrefixPos = ipResp.indexOf("+CGPADDR: 1,");
  bool hasIP = false;
  if (ipPrefixPos >= 0) {
    int ipStart = ipPrefixPos + strlen("+CGPADDR: 1,");
    if (ipStart < (int)ipResp.length()) {
      char c = ipResp.charAt(ipStart);
      // IP kosong biasanya tampil sebagai "0.0.0.0" atau langsung koma/CR/LF
      hasIP = (c != ',' && c != '\r' && c != '\n');
      if (ipResp.substring(ipStart).startsWith("0.0.0.0")) hasIP = false;
    }
  }

  if (!hasIP) {
    Serial.println("    -> BELUM dapat IP address. PDP context gagal aktif.");
    Serial.println("       Cek APN benar/tidak, atau kartu SIM memang tidak punya paket data.");
    return false;
  }
  Serial.println("    -> Dapat IP address. Modul terhubung ke jaringan data operator.");

  // Tes 1: pakai domain (neverssl.com sengaja tidak redirect ke HTTPS, jadi
  // hasil 200 = pasti valid, beda dengan google.com yang bisa redirect 301).
  Serial.println("-- Tes 1: HTTP GET ke http://neverssl.com (domain, lewat DNS) --");
  int statusCode = doHttpGet("http://neverssl.com");
  if (statusCode >= 0) {
    Serial.print("    -> Status code: "); Serial.print(statusCode);
    Serial.print(" => "); Serial.println(interpretHttpStatus(statusCode));
  } else {
    Serial.println("    -> Tidak menangkap respons +HTTPACTION sama sekali (timeout total).");
  }

  bool httpOk = (statusCode == 200);

  // Kalau tes 1 gagal karena masalah koneksi (bukan DNS), coba lagi pakai
  // IP langsung (Cloudflare 1.1.1.1) untuk memisahkan penyebab:
  //  - Tes 2 ikut gagal juga -> kemungkinan besar APN/SIM tidak diizinkan
  //    akses internet umum (banyak SIM IoT/M2M dibatasi hanya ke server
  //    tertentu), atau operator/firewall blokir trafik keluar.
  //  - Tes 2 berhasil (200) -> neverssl.com spesifik yang bermasalah/diblokir,
  //    internet secara umum sebenarnya JALAN.
  if (!httpOk) {
    Serial.println("-- Tes 2: HTTP GET ke http://1.1.1.1 (IP langsung, tanpa DNS) --");
    int statusCode2 = doHttpGet("http://1.1.1.1");
    if (statusCode2 >= 0) {
      Serial.print("    -> Status code: "); Serial.print(statusCode2);
      Serial.print(" => "); Serial.println(interpretHttpStatus(statusCode2));
    } else {
      Serial.println("    -> Tidak menangkap respons +HTTPACTION sama sekali (timeout total).");
    }

    if (statusCode2 == 200 || statusCode2 == 400) {
      // Cloudflare balas 400 untuk request tanpa Host header yang tepat,
      // tapi itu tetap bukti request sampai ke server -> internet JALAN.
      Serial.println("    -> Internet sebenarnya JALAN (tembus ke IP luar). Kemungkinan besar");
      Serial.println("       neverssl.com spesifik yang diblokir/lambat/down, bukan modul kita.");
      httpOk = true;
    } else {
      Serial.println("    -> Tes ke IP langsung JUGA gagal. Kemungkinan besar:");
      Serial.println("       1) Kartu SIM/APN dibatasi (SIM IoT/M2M sering hanya boleh akses");
      Serial.println("          server tertentu, tidak internet umum) - tanya provider SIM-nya.");
      Serial.println("       2) Operator/firewall blokir trafik data keluar dari APN ini.");
      Serial.println("       3) Sinyal terlalu lemah/tidak stabil saat proses connect (cek AT+CSQ).");
    }
  }

  if (httpOk) {
    Serial.println("    -> INTERNET AKTIF & BISA DIPAKAI.");
  } else {
    Serial.println("    -> Belum terkonfirmasi tembus internet. Lihat arti kode di atas untuk langkah selanjutnya.");
  }

  return httpOk;
}

void enableGPS() {
  Serial.println("-- Mengaktifkan GPS --");
  Serial.println(sendAT("AT+CGPS=1", 3000)); // enable GNSS
  delay(2000);
}

// Parsing sederhana hasil AT+CGPSINFO menjadi field lat/long/altitude.
bool getGPSLocation(String &raw) {
  String resp = sendAT("AT+CGPSINFO", 3000);
  raw = resp;

  // Format sukses: +CGPSINFO: lat,N/S,lon,E/W,date,time,alt,speed,course
  if (resp.indexOf("+CGPSINFO:") < 0) return false;
  if (resp.indexOf(",,,,") >= 0) return false; // belum ada fix GPS

  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  ModemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  Serial.println();
  Serial.println("=== SIM7600G-H Init (4G LTE + GPS) ===");

  powerOnModem();

  if (!waitModemReady()) {
    Serial.println("Modem tidak merespon. Cek wiring & power supply.");
    return;
  }

  Serial.println("Modem siap.");
  sendAT("ATE0"); // matikan echo

  runDiagnostics();   // jalankan dulu untuk troubleshooting LED NET kedip cepat
  checkNetworkStatus();
  checkInternetConnection("internet"); // ganti APN sesuai kartu SIM yang dipakai
  enableGPS();
}

void loop() {
  String gpsRaw;
  if (getGPSLocation(gpsRaw)) {
    Serial.println("-- Lokasi GPS --");
    Serial.println(gpsRaw);
  } else {
    Serial.println("Menunggu fix GPS...");
  }

  delay(5000);
}

/*
 * ============================================================
 * DAFTAR AT COMMAND YANG SERING DIPAKAI (SIMCOM SIM7600 series)
 * ============================================================
 * Kirim manual lewat Serial Monitor (set line ending "Both NL & CR"
 * / "CR+LF") kalau mau uji coba satu-satu, atau lewat fungsi sendAT().
 *
 * -- Dasar / Info Modul --
 *   AT                  - tes modul merespon (balas "OK")
 *   ATI                 - info model modul
 *   AT+CGMR             - versi firmware
 *   AT+GSN / AT+CGSN    - nomor IMEI modul
 *   ATE0 / ATE1         - matikan/nyalakan command echo
 *   AT&F                - reset ke pengaturan pabrik
 *   AT+CFUN?            - cek mode fungsi radio (0=minimum,1=full,4=flight)
 *   AT+CFUN=1,1         - reset modul (full functionality + restart)
 *
 * -- SIM Card --
 *   AT+CPIN?            - status SIM (READY / SIM PIN / SIM PUK)
 *   AT+CPIN="1234"      - masukkan PIN SIM jika terkunci
 *   AT+CCID             - baca ICCID kartu SIM (bukti SIM terbaca)
 *   AT+CIMI             - baca IMSI kartu SIM
 *
 * -- Sinyal & Jaringan --
 *   AT+CSQ              - kekuatan sinyal (RSSI, BER)
 *   AT+CREG?            - status registrasi jaringan 2G/3G
 *   AT+CEREG?           - status registrasi jaringan LTE
 *   AT+COPS?            - operator yang sedang terdaftar
 *   AT+COPS=?           - scan semua operator yang terdeteksi (lambat!)
 *   AT+COPS=0           - registrasi otomatis ke operator
 *   AT+CNMP?            - cek mode jaringan aktif
 *   AT+CNMP=2           - set mode Auto (2=Auto,13=GSM,38=LTE,51=GSM+LTE)
 *   AT+CPSI?            - info detail sel/jaringan yang terhubung
 *
 * -- APN / Data (PDP Context) --
 *   AT+CGDCONT?                       - lihat konfigurasi APN saat ini
 *   AT+CGDCONT=1,"IP","internet"      - set APN manual (contoh)
 *   AT+CGACT=1,1                      - aktifkan PDP context 1
 *   AT+CGACT?                         - cek status PDP context aktif/tidak
 *   AT+CGPADDR=1                      - lihat IP address yang didapat (bukti dapat data)
 *   AT+NETOPEN                        - buka koneksi network stack (alternatif AT+CGACT)
 *   AT+IPADDR                         - cek IP address (dipakai bersama AT+NETOPEN)
 *
 * -- Tes Internet Sungguhan (HTTP) --
 *   AT+HTTPINIT                       - mulai sesi HTTP
 *   AT+HTTPPARA="CID",1               - pakai PDP context 1 untuk HTTP
 *   AT+HTTPPARA="URL","http://..."    - set URL tujuan
 *   AT+HTTPACTION=0                   - eksekusi GET (0=GET,1=POST,2=HEAD)
 *                                        balasan URC: +HTTPACTION: 0,<status>,<len>
 *                                        status 200 = internet benar-benar nyambung
 *   AT+HTTPREAD                       - baca isi response body
 *   AT+HTTPTERM                       - tutup sesi HTTP
 *   AT+PING="8.8.8.8"                 - ping IP (jika firmware mendukung), cara cepat
 *                                        tes tembus internet tanpa HTTP
 *
 * -- Arti <statuscode> pada balasan +HTTPACTION (sumber: SIMCOM "SIM7500_
 *    SIM7600_SIM7800 Series_HTTP_AT Command Manual", bab 4 & 5) --
 *    Modul memakai gabungan dua tabel berikut untuk field yang sama:
 *
 *    Tabel HTTP response code (bab 4, sebagian):
 *      200 OK                          301/302/303/307 Redirect
 *      400 Bad Request                 403 Forbidden
 *      404 Not Found                   500-504 Server Error
 *      600 Not HTTP PDU                601 Network Error
 *      602 No memory                   603 DNS Error
 *      604 Stack Busy
 *
 *    Tabel HTTP error code internal modul (bab 5):
 *      0   Success                     701 Alert state
 *      702 Unknown error               703 Busy (sesi lama belum ditutup)
 *      704 Connection closed error     705 Timeout
 *      706 Send/recv socket gagal      707 File/memory error
 *      708 Parameter tidak valid       709 Network error
 *      710 SSL session gagal mulai     711 Wrong state
 *      712 Gagal buat socket           713 Get DNS failed
 *      714 Connect socket gagal        715 Handshake gagal (SSL)
 *      716 Close socket gagal          717 No network error
 *      718 Send data timeout           719 CA missed (sertifikat SSL)
 *
 *    Lihat fungsi interpretHttpStatus() di atas untuk versi terjemahan
 *    Indonesia otomatis yang dipakai program ini.
 *
 * -- Power / Baterai --
 *   AT+CBC              - status baterai/tegangan (jika didukung board)
 *   AT+CPOWD=1           - matikan modul secara software (graceful power down)
 *
 * -- GPS / GNSS --
 *   AT+CGPS=1            - aktifkan GNSS
 *   AT+CGPS=0            - matikan GNSS
 *   AT+CGPS?             - cek status GNSS aktif/tidak
 *   AT+CGPSINFO           - baca data lokasi GPS (lat,lon,tanggal,waktu,dll)
 *   AT+CGNSSMODE=?        - cek mode konstelasi GNSS yang didukung (GPS/GLONASS/dll)
 *
 * -- SMS (opsional, jika dibutuhkan) --
 *   AT+CMGF=1                        - set mode SMS text
 *   AT+CMGS="+62812xxxxxxx"          - kirim SMS (diikuti isi pesan lalu Ctrl+Z)
 *   AT+CMGL="ALL"                    - list semua SMS di memori
 *
 * Referensi lengkap: SIMCOM "SIM7600 Series AT Command Manual" (PDF resmi
 * dari SIMCOM/reseller modul).
 * ============================================================
 */
