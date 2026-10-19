package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String command = commands.take();
                long arrival = System.nanoTime();

                String reply;
                try {
                    reply = execute(command);
                } catch (NumberFormatException e) {
                    reply = "ERROR parametru invalid (nu este numar)";
                } catch (RuntimeException e) {
                    reply = "ERROR " + e.getMessage();
                }

                mqtt.publish(Config.statusTopic(deviceId),
                        deviceId + ";" + System.currentTimeMillis() + ";" + reply);

                long responseMs = (System.nanoTime() - arrival) / 1_000_000;
                System.out.printf("   [aperiodic] \"%s\" -> %s, R = %d ms%n", command, reply, responseMs);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private String execute(String command) {
        String[] parts = command.trim().split("\\s+");
        String name = parts[0].toUpperCase();

        switch (name) {
            case "SET_INTERVAL": {
                if (parts.length != 2) {
                    throw new IllegalArgumentException("SET_INTERVAL cere un parametru: <ms>");
                }
                long ms = Long.parseLong(parts[1]);
                if (ms <= 0) {
                    throw new IllegalArgumentException("intervalul trebuie sa fie pozitiv");
                }
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            }
            case "SET_THRESHOLD": {
                if (parts.length != 2) {
                    throw new IllegalArgumentException("SET_THRESHOLD cere un parametru: <valoare>");
                }
                double value = Double.parseDouble(parts[1]);
                periodic.setThreshold(value);
                return "OK SET_THRESHOLD " + String.format(Locale.US, "%.2f", value);
            }
            case "RESET":
                sensor.reset();
                return "OK RESET";
            case "STATUS":
                return "OK STATUS interval=" + periodic.getPeriodMs()
                        + " prag=" + String.format(Locale.US, "%.2f", periodic.getThreshold());
            default:
                throw new IllegalArgumentException("comanda necunoscuta: " + parts[0]);
        }
    }
}
