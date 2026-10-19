package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String message = inbox.take();
                try {
                    process(message);
                } catch (RuntimeException e) {
                    System.out.println("[" + kind + "] mesaj invalid: " + message);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void process(String message) {
        storage.append(kind, message);

        switch (kind) {
            case "telemetry": {
                String[] fields = message.split(";");
                String deviceId = fields[0];
                double temperature = Double.parseDouble(fields[2]);
                dashboard.updateTelemetry(deviceId, temperature);
                break;
            }
            case "alarm": {
                String[] fields = message.split(";");
                String deviceId = fields[0];
                double temperature = Double.parseDouble(fields[2]);
                double threshold = Double.parseDouble(fields[3]);
                dashboard.updateAlarm(deviceId);
                System.out.printf("[ALARMA] %s: temperatura %.2f depaseste pragul %.2f%n",
                        deviceId, temperature, threshold);
                break;
            }
            case "status": {
                String[] fields = message.split(";", 3);
                String deviceId = fields[0];
                String text = fields[2];
                dashboard.updateStatus(deviceId);
                System.out.println("[STATUS] " + deviceId + ": " + text);
                break;
            }
            default:
                throw new IllegalArgumentException("tip necunoscut: " + kind);
        }
    }
}
