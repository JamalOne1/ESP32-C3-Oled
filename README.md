I got this https://www.aliexpress.com/item/1005007342383107.html?spm=a2g0o.order_list.order_list_main.23.79c21802qtrhl5

And made a little program. To send text and other commands to it via Serial terminal:

ESP32-C3 + 0.42" SSD1306 (72x40) OLED

  Features
  - Lower ticker with commands:
      f/1..3  font small/medium/large (Nordic-ready Helvetica)
      s/0     stop scroll (non-blocking)
      s/1..9  scroll speed (1=slowest, 9=fastest)
      h/1     show ticker
      h/0     hide ticker (static centered text)
  - Full-screen clock modes:
      t/1     show time (HH:MM:SS)
      t/2     show time + date (YYYY-MM-DD) on the line under time
      t/0     (or any other command/text) hides time until t/1 or t/2
      tz/N    manual timezone offset in HOURS (e.g., tz/2, tz/-1)
      ts/HH:MM:SS  set manual time (used when no valid NTP)
  - Wi-Fi via serial (optional):
      w/SSID        save SSID
      wp/PASS       save password
      wc/           connect now (NTP auto if connected)
      wf/           forget saved Wi-Fi
  - Serial: 115200 baud, newline '\n'
  - Pins:   SDA=5, SCL=6, I2C addr 0x3C
