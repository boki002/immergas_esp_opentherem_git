  ESP32 + OpenTherm padlófűtés / radiátor szabályzó
  -------------------------------------------------

  A rendszer két külön fűtési kört irányít:
  - padlófűtés (35–42 °C tartomány)
  - radiátoros kör (45–65 °C tartomány)

  A két kör bemenete (IN1/IN2) külső kontaktokkal (NO/relé) vezérelhető.
  A kívánt előremenő hőmérsékleteket két potméter (ADC) állítja.

  MŰKÖDÉSI LOGIKA:
  ----------------
  • Ha valamelyik zóna aktív, a kazán CH üzem engedélyezésre kerül.
  • A célhőmérsékletet a zónák alapján választja:
      - csak padló aktív → padlóhőfok
      - csak radiátor aktív → radiátorhőfok
      - mindkettő aktív → a radiátor hőfok (zóna szelep zárja padló kört)
  
  • Radiátoros kör indításakor „lágy indítást” alkalmaz:
      3 percig csak minimális radiátor előremenőt kér (soft start).

  • HMV prioritás:
      A kazánból érkező OpenTherm válaszból kiolvassuk,
      hogy éppen melegvizet készít-e.
      Ha HMV aktív → CH parancsok nem kerülnek kiküldésre.
      (Teljes HMV elsőbbség fűtéssel szemben.)
      MHV 50C fokra állítva

  • OpenTherm kommunikáció 1 Hz ciklussal:
      - CH/HMV státusz beállítása
      - hőfokküldés (csak ha nem HMV)
      - OT válasz státusz kiértékelése (debug + hibafelügyelet)

  • OLED kijelző mutatja:
      - beállított hőfokokat
      - aktív zónát
      - kazán üzemmódot (OFF / CH / HMV)
      - küldött előremenő hőmérsékletet
      - radiátor soft start visszaszámlálást

  A kód biztosítja a zavartalan működést, a két kör közti prioritásokat,
  valamint a biztonságos HMV elsőbbséget a fűtéssel szemben.
