package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;
    private final PeriodicTelemetryTask periodic;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents,
                             PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        long lastRelease = 0;
        boolean hasRelease = false;

        try {
            while (true) {
                double temperature = alarmEvents.take();
                long arrival = System.nanoTime();

                if (hasRelease) {
                    long sinceLastMs = (arrival - lastRelease) / 1_000_000;
                    if (sinceLastMs < MIN_INTERARRIVAL_MS) {
                        System.out.printf("   [sporadic] alarma RESPINSA (temp = %.2f): au trecut doar %d ms "
                                + "(interval minim %d ms)%n", temperature, sinceLastMs, MIN_INTERARRIVAL_MS);
                        continue;
                    }
                }
                lastRelease = arrival;
                hasRelease = true;

                String message = deviceId + ";" + System.currentTimeMillis() + ";"
                        + String.format(Locale.US, "%.2f", temperature) + ";"
                        + String.format(Locale.US, "%.2f", periodic.getThreshold());
                mqtt.publish(Config.alarmTopic(deviceId), message);

                long responseMs = (System.nanoTime() - arrival) / 1_000_000;
                System.out.printf("   [sporadic] alarma publicata (temp = %.2f), R = %d ms%n",
                        temperature, responseMs);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
