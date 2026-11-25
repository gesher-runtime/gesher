package android.os;
public final class Environment {
    public static final String DIRECTORY_MUSIC = "Music", DIRECTORY_PODCASTS = "Podcasts",
            DIRECTORY_RINGTONES = "Ringtones", DIRECTORY_ALARMS = "Alarms",
            DIRECTORY_NOTIFICATIONS = "Notifications", DIRECTORY_PICTURES = "Pictures",
            DIRECTORY_MOVIES = "Movies", DIRECTORY_DOWNLOADS = "Download", DIRECTORY_DCIM = "DCIM",
            DIRECTORY_DOCUMENTS = "Documents", DIRECTORY_AUDIOBOOKS = "Audiobooks",
            DIRECTORY_RECORDINGS = "Recordings";
    /** Environment.STANDARD_DIRECTORIES, android-16.0.0_r4. */
    public static final String[] STANDARD_DIRECTORIES = {DIRECTORY_MUSIC, DIRECTORY_PODCASTS,
            DIRECTORY_RINGTONES, DIRECTORY_ALARMS, DIRECTORY_NOTIFICATIONS, DIRECTORY_PICTURES,
            DIRECTORY_MOVIES, DIRECTORY_DOWNLOADS, DIRECTORY_DCIM, DIRECTORY_DOCUMENTS,
            DIRECTORY_AUDIOBOOKS, DIRECTORY_RECORDINGS};
    private Environment() {}
}
