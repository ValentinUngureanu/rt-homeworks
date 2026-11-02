package ssatr.service;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat.
 */

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

public class Dashboard {

    private static class DeviceState {
        double lastTemperature = Double.NaN;
        long lastMessageMs;
        int alarmCount;
    }

    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState stateOf(String deviceId) {
        return devices.computeIfAbsent(deviceId, id -> new DeviceState());
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        DeviceState state = stateOf(deviceId);
        state.lastTemperature = temperature;
        state.lastMessageMs = System.currentTimeMillis();
    }

    public synchronized void updateAlarm(String deviceId) {
        DeviceState state = stateOf(deviceId);
        state.alarmCount++;
        state.lastMessageMs = System.currentTimeMillis();
    }

    public synchronized void updateStatus(String deviceId) {
        stateOf(deviceId).lastMessageMs = System.currentTimeMillis();
    }

    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println("Niciun device nu a trimis mesaje inca.");
            return;
        }
        long now = System.currentTimeMillis();
        System.out.printf("%-12s %-8s %-16s %s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");
        for (Map.Entry<String, DeviceState> entry : devices.entrySet()) {
            DeviceState state = entry.getValue();
            String temperature = Double.isNaN(state.lastTemperature)
                    ? "-" : String.format(Locale.US, "%.2f", state.lastTemperature);
            String age = String.format(Locale.US, "acum %.1f s", (now - state.lastMessageMs) / 1000.0);
            System.out.printf("%-12s %-8s %-16s %d%n", entry.getKey(), temperature, age, state.alarmCount);
        }
    }
}
