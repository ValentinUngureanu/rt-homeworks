// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Restul îl scrieți voi: TODO 0 ... TODO 3.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de un
//   mutex (xSemaphoreCreateMutex). De exemplu un struct cu:
//   - faza curentă
//   - momentul intrării în faza curentă (millis()), din care se calculează
//     cât a durat faza și cât mai are
//   - cererea pietonului (lampa AȘTEPTAȚI): setată de taskul sporadic,
//     ștearsă de taskul de control la intrarea în PED_GREEN
//   Mutexul se ține cât mai puțin: în el doar citiți sau scrieți câmpurile
//   (eventual copiați starea într-o variabilă locală). Fără mqtt_publish(),
//   Serial sau așteptări cât timp țineți mutexul.
struct TrafficState {
    Phase phase;
    uint32_t phase_entered;
    bool ped_request;
};

static TrafficState state;
static SemaphoreHandle_t state_mutex;

struct ButtonPress {
    TickType_t at;
};
static QueueHandle_t button_queue;

const UBaseType_t PRIO_CONTROL = 6;
const UBaseType_t PRIO_OUTPUT = 5;
const UBaseType_t PRIO_BUTTON = 4;
const uint32_t TASK_STACK_BYTES = 4096;

// TODO 1: TASK PERIODIC DE CONTROL, cu perioada CONTROL_PERIOD_MS.
//   La fiecare job, cu mutexul luat, decide dacă faza curentă s-a terminat și
//   trece în faza următoare, după tabelul din README.md:
//     CAR_GREEN  -> CAR_YELLOW  după CAR_GREEN_MIN_MS, doar dacă există o cerere
//     CAR_YELLOW -> ALL_RED     după CAR_YELLOW_MS
//     ALL_RED    -> PED_GREEN   după ALL_RED_MS; aici se șterge cererea pietonului
//     PED_GREEN  -> CAR_GREEN   după PED_GREEN_MS
//   La fiecare schimbare de fază notați momentul intrării în faza nouă.
//   Taskul nu publică nimic: doar actualizează starea. După ce eliberează
//   mutexul, afișează pe Serial faza nouă, dacă s-a schimbat.
//   Eliberările respectă grila r_k = r_0 + kT: așteptare absolută (xTaskDelayUntil).
//   Durata unei faze se măsoară cu un timp monoton (millis()), nu cu epoch_ms().

static void control_task(void* arg) {
    (void)arg;
    TickType_t last_release = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(CONTROL_PERIOD_MS);

    for (;;) {
        xTaskDelayUntil(&last_release, period);
        bool changed = false;
        Phase new_phase;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        uint32_t now = millis();
        uint32_t elapsed = now - state.phase_entered;
        bool done = false;
        switch (state.phase) {
            case CAR_GREEN:  done = elapsed >= CAR_GREEN_MIN_MS && state.ped_request; break;
            case CAR_YELLOW: done = elapsed >= CAR_YELLOW_MS; break;
            case ALL_RED:    done = elapsed >= ALL_RED_MS; break;
            case PED_GREEN:  done = elapsed >= PED_GREEN_MS; break;
        }
        if (done) {
            switch (state.phase) {
                case CAR_GREEN:  state.phase = CAR_YELLOW; break;
                case CAR_YELLOW: state.phase = ALL_RED; break;
                case ALL_RED:
                    state.phase = PED_GREEN;
                    state.ped_request = false;
                    break;
                case PED_GREEN:  state.phase = CAR_GREEN; break;
            }
            state.phase_entered = now;
            changed = true;
        }
        new_phase = state.phase;
        xSemaphoreGive(state_mutex);

        if (changed) {
            Serial.printf("[control] faza nouă: %s\n", PHASE_NAMES[new_phase]);
        }
    }
}

// TODO 2: TASK PERIODIC DE IEȘIRE, cu perioada OUTPUT_PERIOD_MS.
//   La fiecare job:
//   - copiază starea (cu mutexul luat, cât mai scurt)
//   - stabilește culorile celor două lămpi din faza curentă (tabelul din README.md)
//   - calculează secundele rămase din verdele pietonilor: în PED_GREEN,
//     rotunjite în sus (7,2 s rămase -> 8); 0 în celelalte faze
//   - publică: mqtt_publish("lights", "deviceId;timestamp;faza;masini;pietoni;asteptati;secunde")
//     de exemplu "esp32-a1b2c3;1727600000000;PED_GREEN;RED;GREEN;0;8"
//   - aprinde LED-ul de pe placă (LED_PIN, aprins pe LOW) cât timp lampa
//     AȘTEPTAȚI este aprinsă
//   Așteptare absolută (xTaskDelayUntil), ca și la taskul de control.
static void output_task(void* arg) {
    (void)arg;
    TickType_t last_release = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(OUTPUT_PERIOD_MS);

    for (;;) {
        xTaskDelayUntil(&last_release, period);

        TrafficState snap;
        uint32_t now;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        snap = state;
        now = millis();
        xSemaphoreGive(state_mutex);

        Light cars, peds;
        switch (snap.phase) {
            case CAR_GREEN:  cars = GREEN;  peds = RED;   break;
            case CAR_YELLOW: cars = YELLOW; peds = RED;   break;
            case ALL_RED:    cars = RED;    peds = RED;   break;
            default:         cars = RED;    peds = GREEN; break;
        }

        unsigned seconds = 0;
        if (snap.phase == PED_GREEN) {
            uint32_t elapsed = now - snap.phase_entered;
            uint32_t remaining_ms = elapsed >= PED_GREEN_MS ? 0 : PED_GREEN_MS - elapsed;
            seconds = (remaining_ms + 999) / 1000;  // rotunjit în sus
            if (seconds == 0) seconds = 1;
        }

        char msg[128];
        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%u",
                 DEVICE_ID, (long long)epoch_ms(), PHASE_NAMES[snap.phase],
                 LIGHT_NAMES[cars], LIGHT_NAMES[peds], snap.ped_request ? 1 : 0, seconds);
        mqtt_publish("lights", msg);

        digitalWrite(LED_PIN, snap.ped_request ? LOW : HIGH);  // aprins pe LOW
    }
}

// TODO 3: TASK SPORADIC, tratează apăsările butonului pentru pietoni.
//   Apăsările vin din două surse, în aceeași coadă (coada butonului):
//   - butonul BOOT de pe placă, printr-o rutină de întrerupere (ISR, mai jos)
//   - butonul virtual din dashboard: comanda "BUTTON", pusă în coadă de
//     command_received()
//   Taskul:
//   - așteaptă o apăsare din coada butonului
//   - dacă de la ultima apăsare acceptată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, o respinge (mesaj pe Serial). Așa se filtrează și
//     contactele imperfecte ale butonului (bounce), care produc mai multe
//     întreruperi la o singură apăsare.
//   - altfel înregistrează cererea pietonului în starea semaforului (cu mutexul)
//     și afișează pe Serial că a acceptat-o
//   Momentul sosirii se ia când apăsarea intră în coadă (în ISR sau în
//   command_received()), nu când o preia taskul, și se pune în elementul cozii.

// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT, înregistrată în setup() cu
//   attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING).
//   Scurtă: pune o apăsare în coada butonului cu xQueueSendFromISR() și atât.
//   Fără Serial, fără mqtt_publish(), fără mutex. Se declară cu IRAM_ATTR:
//
//   void IRAM_ATTR button_isr(void) { ... }

static void button_task(void* arg) {
    (void)arg;
    bool have_accepted = false;
    TickType_t last_accepted = 0;

    for (;;) {
        ButtonPress press;
        if (xQueueReceive(button_queue, &press, portMAX_DELAY) != pdTRUE) continue;

        if (have_accepted && (TickType_t)(press.at - last_accepted) < pdMS_TO_TICKS(MIN_INTERARRIVAL_MS)) {
            Serial.println("[buton] apăsare respinsă (prea deasă)");
            continue;
        }
        have_accepted = true;
        last_accepted = press.at;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        state.ped_request = true;
        xSemaphoreGive(state_mutex);

        Serial.println("[buton] cerere pietoni înregistrată");
    }
}

void IRAM_ATTR button_isr(void) {
    ButtonPress press = {xTaskGetTickCountFromISR()};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);
    portYIELD_FROM_ISR(woken);
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului (o apăsare, la fel ca cea din
    //   ISR), fără să vă blocați (timeout 0). Celelalte comenzi se ignoră:
    //   în această temă placa nu are alte comenzi.
        if (strcmp(text, "BUTTON") == 0) {
            ButtonPress press = {xTaskGetTickCount()};
            if (xQueueSend(button_queue, &press, 0) != pdTRUE) {
                Serial.println("[mqtt] coada butonului este plină");
            }
        }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): mutexul și coada, înainte de mqtt_start()
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(8, sizeof(ButtonPress));
    state.phase = CAR_GREEN;
    state.phase_entered = millis();
    state.ped_request = false;

    mqtt_start();

    // TODO 0 (b): taskurile, apoi întreruperea
    xTaskCreate(control_task, "control", TASK_STACK_BYTES, NULL, PRIO_CONTROL, NULL);
    xTaskCreate(output_task, "output", TASK_STACK_BYTES, NULL, PRIO_OUTPUT, NULL);
    xTaskCreate(button_task, "button", TASK_STACK_BYTES, NULL, PRIO_BUTTON, NULL);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
