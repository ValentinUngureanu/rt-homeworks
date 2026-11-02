// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Taskurile le scrieți voi: TODO 0 ... TODO 3.
// Exemplele de pornire sunt în laboratorul 0 din rt-labs-esp32c3; vezi
// exemple/README.md din folderul temei.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// TODO 1: TASK PERIODIC, cu perioada T (inițial PERIOD_MS_INITIAL).
//   Fiecare job:
//   - citește temperatura: read_temperature()
//   - publică telemetria: mqtt_publish("telemetry", "deviceId;timestamp;temperatura")
//     (timestamp = epoch_ms(), tipărit cu %lld; temperatura cu "%.2f")
//   - dacă temperatura depășește pragul (inițial THRESHOLD_INITIAL), pune
//     un eveniment în coada de alarme, fără să se blocheze (timeout 0).
//     Taskul periodic NU tratează alarma: o tratează taskul sporadic.
//   Eliberările respectă grila r_k = r_0 + kT: folosiți așteptare absolută
//   (xTaskDelayUntil), nu relativă (delay, vTaskDelay), ca să nu apară derivă.
//   Perioada și pragul se schimbă prin comenzi (taskul aperiodic), deci sunt
//   variabile globale volatile, citite la fiecare job.

volatile uint32_t period_ms = PERIOD_MS_INITIAL;
volatile float threshold = THRESHOLD_INITIAL;

const uint32_t PERIOD_MS_MIN = 10;

struct AlarmEvent {
    float temperature;
    float threshold;
};

struct Command {
    char text[COMMAND_MAX_LEN];
};

QueueHandle_t alarm_queue;
QueueHandle_t command_queue;

const UBaseType_t PRIORITY_PERIODIC = 5;
const UBaseType_t PRIORITY_SPORADIC = 4;
const UBaseType_t PRIORITY_APERIODIC = 3;
const uint32_t TASK_STACK = 4096;

void task_periodic(void* parameter) {
    TickType_t last_release = xTaskGetTickCount();
    uint32_t job = 0;
    char message[96];

    for (;;) {
        xTaskDelayUntil(&last_release, pdMS_TO_TICKS(period_ms));

        uint32_t job_start = millis();
        float temperature = read_temperature();

        snprintf(message, sizeof(message), "%s;%lld;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), temperature);
        mqtt_publish("telemetry", message);

        float current_threshold = threshold;
        if (temperature > current_threshold) {
            AlarmEvent event = { temperature, current_threshold };
            if (xQueueSend(alarm_queue, &event, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme este plina, eveniment pierdut");
            }
        }

        Serial.printf("[periodic] job %lu: temp = %.2f, C = %lu ms\n",
                      (unsigned long)job, temperature, (unsigned long)(millis() - job_start));
        job++;
    }
}

// TODO 2: TASK SPORADIC, tratează alarmele de temperatură.
//   - așteaptă un eveniment din coada de alarme
//   - dacă de la ultima alarmă publicată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, respinge evenimentul (afișați un mesaj pe Serial)
//   - altfel publică: mqtt_publish("alarm", "deviceId;timestamp;temperatura;prag")
//   Pentru intervale folosiți un timp monoton, millis(), nu epoch_ms().

void task_sporadic(void* parameter) {
    uint32_t last_release = 0;
    bool has_release = false;
    char message[112];
    AlarmEvent event;

    for (;;) {
        xQueueReceive(alarm_queue, &event, portMAX_DELAY);
        uint32_t arrival = millis();

        if (has_release && (uint32_t)(arrival - last_release) < MIN_INTERARRIVAL_MS) {
            Serial.printf("   [sporadic] alarma RESPINSA (temp = %.2f): au trecut doar %lu ms (interval minim %lu ms)\n",
                          event.temperature, (unsigned long)(arrival - last_release),
                          (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        last_release = arrival;
        has_release = true;

        snprintf(message, sizeof(message), "%s;%lld;%.2f;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), event.temperature, event.threshold);
        mqtt_publish("alarm", message);

        Serial.printf("   [sporadic] alarma publicata (temp = %.2f, prag = %.2f), R = %lu ms\n",
                      event.temperature, event.threshold, (unsigned long)(millis() - arrival));
    }
}

// TODO 3: TASK APERIODIC, execută comenzile primite de la business service.
//   Comenzile ajung în command_received() (mai jos), care le pune în coada de
//   comenzi; fiecare comandă eliberează un job al taskului:
//     SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
//     SET_THRESHOLD <valoare> schimbă pragul de alarmă
//     RESET                   readuce perioada și pragul la valorile inițiale
//     STATUS                  raportează perioada și pragul curente
//   După fiecare comandă publică o confirmare:
//     mqtt_publish("status", "deviceId;timestamp;OK SET_INTERVAL 1000")
//   O comandă necunoscută sau un parametru invalid nu oprește taskul: se
//   publică "ERROR <motiv>".

static void execute_command(const char* text, char* reply, size_t reply_size) {
    char buffer[COMMAND_MAX_LEN];
    snprintf(buffer, sizeof(buffer), "%s", text);

    char* save = NULL;
    char* name = strtok_r(buffer, " \t\r\n", &save);
    char* arg = strtok_r(NULL, " \t\r\n", &save);
    char* extra = strtok_r(NULL, " \t\r\n", &save);

    if (name == NULL) {
        snprintf(reply, reply_size, "ERROR comanda goala");
    } else if (strcasecmp(name, "SET_INTERVAL") == 0) {
        char* end = NULL;
        long value = (arg != NULL) ? strtol(arg, &end, 10) : 0;
        if (arg == NULL || extra != NULL) {
            snprintf(reply, reply_size, "ERROR SET_INTERVAL cere un parametru: <ms>");
        } else if (*end != '\0') {
            snprintf(reply, reply_size, "ERROR parametru invalid (nu este numar)");
        } else if (value < (long)PERIOD_MS_MIN) {
            snprintf(reply, reply_size, "ERROR intervalul trebuie sa fie cel putin %lu ms",
                     (unsigned long)PERIOD_MS_MIN);
        } else {
            period_ms = (uint32_t)value;
            snprintf(reply, reply_size, "OK SET_INTERVAL %ld", value);
        }
    } else if (strcasecmp(name, "SET_THRESHOLD") == 0) {
        char* end = NULL;
        float value = (arg != NULL) ? strtof(arg, &end) : 0;
        if (arg == NULL || extra != NULL) {
            snprintf(reply, reply_size, "ERROR SET_THRESHOLD cere un parametru: <valoare>");
        } else if (*end != '\0' || !isfinite(value)) {
            snprintf(reply, reply_size, "ERROR parametru invalid (nu este numar)");
        } else {
            threshold = value;
            snprintf(reply, reply_size, "OK SET_THRESHOLD %.2f", value);
        }
    } else if (strcasecmp(name, "RESET") == 0) {
        period_ms = PERIOD_MS_INITIAL;
        threshold = THRESHOLD_INITIAL;
        snprintf(reply, reply_size, "OK RESET");
    } else if (strcasecmp(name, "STATUS") == 0) {
        snprintf(reply, reply_size, "OK STATUS interval=%lu prag=%.2f",
                 (unsigned long)period_ms, (double)threshold);
    } else {
        snprintf(reply, reply_size, "ERROR comanda necunoscuta: %s", name);
    }
}

void task_aperiodic(void* parameter) {
    Command command;
    char reply[160];
    char message[224];

    for (;;) {
        xQueueReceive(command_queue, &command, portMAX_DELAY);
        uint32_t arrival = millis();

        execute_command(command.text, reply, sizeof(reply));

        snprintf(message, sizeof(message), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), reply);
        mqtt_publish("status", message);

        Serial.printf("   [aperiodic] \"%s\" -> %s, R = %lu ms\n",
                      command.text, reply, (unsigned long)(millis() - arrival));
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: puneți comanda în coada de comenzi, fără să vă blocați
    //   (timeout 0). text este valid doar în timpul acestui apel: elementul
    //   cozii trebuie să conțină textul însuși (de exemplu un struct cu
    //   char text[COMMAND_MAX_LEN]), nu un pointer.
    Command command;
    snprintf(command.text, sizeof(command.text), "%s", text);
    if (xQueueSend(command_queue, &command, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi este plina, comanda pierduta");
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
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați cele două cozi (xQueueCreate): coada de alarme și
    //   coada de comenzi. Înainte de mqtt_start(): de acolo încolo pot sosi
    //   comenzi, iar command_received() le pune în coada de comenzi.
    alarm_queue = xQueueCreate(8, sizeof(AlarmEvent));
    command_queue = xQueueCreate(8, sizeof(Command));
    if (alarm_queue == NULL || command_queue == NULL) {
        Serial.println("Eroare: nu s-au putut crea cozile");
        for (;;) delay(1000);
    }

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate). După mqtt_start(),
    //   ca primele mesaje publicate să nu se piardă. Alegeți prioritățile
    //   între 2 și 19: peste loopTask (1), sub taskurile sistemului pentru
    //   evenimente și Wi-Fi (20, 23). Stiva, în octeți: cel puțin 4096.
    xTaskCreate(task_periodic, "periodic", TASK_STACK, NULL, PRIORITY_PERIODIC, NULL);
    xTaskCreate(task_sporadic, "sporadic", TASK_STACK, NULL, PRIORITY_SPORADIC, NULL);
    xTaskCreate(task_aperiodic, "aperiodic", TASK_STACK, NULL, PRIORITY_APERIODIC, NULL);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}