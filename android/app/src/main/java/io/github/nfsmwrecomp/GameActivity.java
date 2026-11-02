package io.github.nfsmwrecomp;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;

/**
 * La actividad del juego (y de la sonda de Vulkan).
 *
 * Corre en su propio proceso (":juego", ver AndroidManifest.xml). Las librerias
 * nativas no se pueden descargar ni volver a cargar en el mismo proceso, asi
 * que cada partida o cada sonda empieza en un proceso limpio y al salir se
 * mata. La pantalla de inicio vive en el proceso principal y no se entera.
 *
 * Los argumentos los monta Ajustes.argumentos() en la pantalla de inicio. Aqui
 * solo se anade uno: la ISO.
 */
public class GameActivity extends SDLActivity {

    static final String EXTRA_ARGUMENTOS = "io.github.nfsmwrecomp.ARGUMENTOS";
    static final String EXTRA_ISO = "io.github.nfsmwrecomp.ISO";
    /** Ajustes.RES_480P, RES_720P o RES_NATIVA. */
    static final String EXTRA_RESOLUCION = "io.github.nfsmwrecomp.RESOLUCION";
    /** Llenar la pantalla entera en vez de 16:9 con barras. */
    static final String EXTRA_ESTIRAR = "io.github.nfsmwrecomp.ESTIRAR";
    /** Dibujar a la proporcion de la pantalla (Ajustes.panoramico): llena la pantalla sin deformar. */
    static final String EXTRA_PANORAMICO = "io.github.nfsmwrecomp.PANORAMICO";
    static final String EXTRA_MOSTRAR_FPS = "io.github.nfsmwrecomp.MOSTRAR_FPS";
    /** El id de la edicion del juego de la ISO (Ediciones); sin el, la principal. */
    static final String EXTRA_EDICION = "io.github.nfsmwrecomp.EDICION";

    private static final String TAG = "nfsmw";

    private String[] argumentos = new String[0];
    /** Cual libmain cargar: la de la edicion de la ISO. */
    private Ediciones.Edicion edicion = Ediciones.principal();

    private final Handler reloj = new Handler(Looper.getMainLooper());
    private TextView textoFps;
    private Runnable actualizarFps;
    private TouchControllerView touchController;

    /** Los fps del juego (tools/parche_fps.py), o -1 si el plugin no ha cargado. */
    private static native float nativeFps();

    /** Milisegundos por fotograma, media del ultimo medio segundo. */
    private static native float nativeMsPorFotograma();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(Idioma.envolver(base));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Intent intent = getIntent();
        ArrayList<String> lista = new ArrayList<>();
        // mw4droid: modo sondeo (setprop debug.nfsmw.survey 1): el dispatcher
        // registra los destinos indirectos sin funcion en vez de abortar. Va
        // aqui, ANTES de que super.onCreate cargue las librerias.
        if (propiedad("debug.nfsmw.survey", null) != null) {
            try {
                Os.setenv("REXGLUE_SURVEY", "1", true);
                Os.setenv("REXGLUE_AVDUMP", "1", true);
            } catch (Exception e) {
                Log.w(TAG, "sin sondeo: " + e);
            }
        }
        // mw4droid: estadisticas de audio (setprop debug.nfsmw.audio 1): el
        // driver de sonido registra cada 5 s, en el log, el pico y el recorte
        // del audio plegado ANTES del limitador. Tambien vale el cvar
        // audio_estadisticas. Va aqui, ANTES de que super.onCreate cargue las
        // librerias, como el sondeo. En vivo las propiedades no puestas leen
        // como cadena vacia, no null; el 0 apaga, como en power/thermal.
        String audioStats = propiedad("debug.nfsmw.audio", null);
        if (audioStats != null && !audioStats.isEmpty() && !audioStats.equals("0")) {
            try {
                Os.setenv("REXGLUE_AUDIOSTATS", "1", true);
                Os.setenv("REX_AUDIO_ESTADISTICAS", "1", true);
            } catch (Exception e) {
                Log.w(TAG, "sin estadisticas de audio: " + e);
            }
        }
        String[] recibidos = intent.getStringArrayExtra(EXTRA_ARGUMENTOS);
        if (recibidos != null) {
            lista.addAll(Arrays.asList(recibidos));
        }

        // LA ISO NO SE COPIA: se le pasa al juego su URI content:// tal cual.
        // El SDK la monta con DiscImageDevice y, cuando toca mapearla, pide el
        // descriptor de vuelta por JNI a openContentFd (mas abajo). Ver
        // tools/parche_iso.py.
        //
        // Pasar /proc/self/fd/N con el descriptor ya abierto NO funciona: open()
        // sobre esa ruta reabre el fichero de verdad, y la app no tiene permiso
        // sobre el almacenamiento compartido.
        String iso = intent.getStringExtra(EXTRA_ISO);
        if (iso != null) {
            lista.add("--game_data_root=" + iso);
        }

        // Apaisado y bloqueado. El manifiesto ya lo pide, pero algunas ROMs y
        // Android 16 se lo saltan; pedirlo aqui tambien es lo que acaba
        // mandando.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        String idEdicion = intent.getStringExtra(EXTRA_EDICION);
        if (idEdicion != null) {
            edicion = Ediciones.porId(idEdicion);
        }

        // Motor nativo: sus carpetas, sus ajustes y sus shaders, antes de que
        // super.onCreate cargue las librerias.
        if (MotorNativo.ACTIVO) {
            MotorNativo.preparar(this, edicion);
            // OpenGL ES pedido para una edicion que no lo trae: con Vulkan.
            if (lista.contains("--nfsmw_nativo_api=gles") && !MotorNativo.glesDisponible(this)) {
                lista.remove("--nfsmw_nativo_api=gles");
                Toast.makeText(this, R.string.gles_no_disponible, Toast.LENGTH_LONG).show();
            }
        }

        // Pantalla ancha: la pantalla entera, tambien la franja de la camara, y su
        // proporcion al juego, que dibuja a ella (nfsmw_proporcion_pantalla).
        boolean panoramico = intent.getBooleanExtra(EXTRA_PANORAMICO, false);
        if (panoramico) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                WindowManager.LayoutParams atributos = getWindow().getAttributes();
                atributos.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                getWindow().setAttributes(atributos);
            }
            lista.add(String.format(Locale.ROOT, "--nfsmw_proporcion_pantalla=%.4f", proporcionPantalla()));
        }

        argumentos = lista.toArray(new String[0]);
        // Release: sin registro (ver Ajustes.argumentos).
        if (BuildConfig.DEBUG) Log.i(TAG, "Argumentos: " + String.join(" ", argumentos));
        super.onCreate(savedInstanceState);

        boolean estirar = intent.getBooleanExtra(EXTRA_ESTIRAR, false) || panoramico;
        ajustarSuperficie(intent.getStringExtra(EXTRA_RESOLUCION), estirar, panoramico);
        if (intent.getBooleanExtra(EXTRA_MOSTRAR_FPS, true)) {
            ponerContadorFps();
        }

        vibracion = new Ajustes(this).vibracion();
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            VibratorManager vm = (VibratorManager) getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            vibrador = vm == null ? null : vm.getDefaultVibrator();
        } else {
            vibrador = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        }
        if (vibrador != null && !vibrador.hasVibrator()) vibrador = null;

        touchController = new TouchControllerView(this);
        // Los toques en los menus van a la imagen del juego, que sin estirar es el
        // 16:9 del centro (ajustarSuperficie).
        touchController.estirar(intent.getBooleanExtra(EXTRA_ESTIRAR, false)
                || intent.getBooleanExtra(EXTRA_PANORAMICO, false));
        mLayout.addView(touchController, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // mw4droid (Mali): medidas y modo de pantalla, sin tocar el resto. Por adb:
        //   adb shell setprop debug.nfsmw.power 1     -> [power] mW cada segundo
        //   adb shell setprop debug.nfsmw.refresh 60  -> modo de pantalla mas cercano a 60 Hz
        //   adb shell setprop debug.nfsmw.thermal 1   -> [thermal] estado termico del sistema
        // En vivo las propiedades no puestas leen como cadena vacia, no null.
        String power = propiedad("debug.nfsmw.power", null);
        if (power != null && !power.isEmpty() && !power.equals("0")) {
            startPowerLog();
        }
        String termico = propiedad("debug.nfsmw.thermal", null);
        if (termico != null && !termico.isEmpty() && !termico.equals("0")) {
            startThermalLog();
        }
        String refresco = propiedad("debug.nfsmw.refresh", null);
        if (refresco != null && !refresco.isEmpty()) {
            pedirModoPantalla(refresco);
        }

        // mw4droid (Dimensity 9500): escalones termicos de degradacion. Por adb:
        //   adb shell setprop debug.nfsmw.thermosteps 1
        // moderate pide 40 fps y severe o mas, 30; none/light suelta el escalon
        // tras 60 s limpios. nfsmw_limite_fps NO se cambia en vivo (el SDK lo
        // declara kRequiresRestart y el hilo del vblank fija su ritmo al
        // arrancar): el escalon se apunta en preferencias ("termico") y en el
        // log, y manda en el SIGUIENTE arranque (Ajustes.limiteFps). Con el
        // flag a cero no se mira nada: comportamiento de siempre.
        String escalones = propiedad("debug.nfsmw.thermosteps", null);
        if (escalones != null && !escalones.isEmpty() && !escalones.equals("0")) {
            startEscalonesTermicos();
        }
    }

    /** El estado termico del sistema y sus cambios: para correlacionar vatios y
     * throttling en las sesiones largas. API 29+. */
    private void startThermalLog() {
        final PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (Build.VERSION.SDK_INT < 29 || pm == null) {
            return;
        }
        PowerManager.OnThermalStatusChangedListener oyente =
                new PowerManager.OnThermalStatusChangedListener() {
            @Override public void onThermalStatusChanged(int estado) {
                Log.i(TAG, "[thermal] " + estado + " " + nombreTermico(estado));
            }
        };
        try {
            pm.addThermalStatusListener(getMainExecutor(), oyente);
            Log.i(TAG, "[thermal] " + pm.getCurrentThermalStatus()
                    + " " + nombreTermico(pm.getCurrentThermalStatus()));
        } catch (RuntimeException e) {
            Log.w(TAG, "sin oyente termico: " + e);
        }
    }

    private static String nombreTermico(int estado) {
        switch (estado) {
            case 0: return "none";
            case 1: return "light";
            case 2: return "moderate";
            case 3: return "severe";
            case 4: return "critical";
            case 5: return "emergency";
            case 6: return "shutdown";
            default: return "?";
        }
    }

    // --- mw4droid (Dimensity 9500): escalones termicos (ver onCreate).

    /** Las preferencias del escalon termico: fichero APARTE del de ajustes
     * porque lo escribe este proceso (":juego") y lo lee la pantalla de inicio
     * en el otro, al montar los argumentos de la siguiente partida. Un fichero
     * que el otro proceso no tenga ya en memoria es lo unico que lee fresco de
     * disco; commit y no apply, por lo mismo. */
    private static final String PREFS_TERMICO = "termico";

    /** Lo que ya ha pedido el escalonador: 0 nada, o 40/30. */
    private volatile int escalonTermico;
    /** La vuelta al limite del usuario, 60 s despues de que el estado despeje. */
    private Runnable vueltaTermica;
    /** El sondeo ya vio margen por encima de cero: getThermalHeadroom devuelve
     * 0 en los moviles sin prevision termica, y ahi un cero no significa calor
     * (no pedir escalon por un cero que no significa nada). */
    private volatile boolean margenVisto;

    /** Aparte de reloj: el onPause lo limpia entero, y la histeresis tiene que
     * sobrevivir a una pausa. */
    private final Handler relojTermico = new Handler(Looper.getMainLooper());

    /** Los escalones termicos: oyente del estado del sistema (API 29+) y sondeo
     * de getThermalHeadroom (API 31+) cada 5 s. Se decide siempre en el hilo
     * principal (evaluarEscalon); el sondeo solo trae datos. */
    private void startEscalonesTermicos() {
        final PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (pm == null) {
            return;
        }
        if (Build.VERSION.SDK_INT < 29) {
            return;  // sin addThermalStatusListener no hay escalones que seguir
        }
        final SharedPreferences p = getSharedPreferences(PREFS_TERMICO, Context.MODE_PRIVATE);

        // Arranca despejado: suelta lo que dejara colgado una sesion anterior
        // (el proceso muere al salir, con el escalon todavia puesto).
        int inicial = pm.getCurrentThermalStatus();
        if (inicial <= 1 && p.contains("thermo_limite_fps")) {
            p.edit().remove("thermo_limite_fps").commit();
            Log.i(TAG, "[thermo] arranca " + nombreTermico(inicial)
                    + ": soltado el escalon de la sesion anterior");
        }

        // El margen de prevision (getThermalHeadroom, API 31+): cuanto hace
        // falta para subir de temperatura en 60 s. En vivo se loguea para
        // correlacionar vatios y throttling; un cero con soporte confirmado
        // (margenVisto) cuenta como moderate antes de que el estado suba.
        if (Build.VERSION.SDK_INT >= 31) {
            Thread hilo = new Thread(new Runnable() { @Override public void run() {
                while (true) {
                    try {
                        float margen = pm.getThermalHeadroom(60);
                        if (margen > 0f) margenVisto = true;
                        final boolean agotado = margenVisto && margen <= 0f;
                        Log.i(TAG, "[thermo] margen a 60 s: " + margen
                                + (agotado ? " (agotado)" : ""));
                        if (agotado) {
                            relojTermico.post(new Runnable() { @Override public void run() {
                                evaluarEscalon(p, pm.getCurrentThermalStatus(), true);
                            }});
                        }
                    } catch (RuntimeException e) {
                        Log.w(TAG, "[thermo] getThermalHeadroom: " + e);
                    }
                    try {
                        Thread.sleep(5000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }}, "nfsmw-thermo");
            hilo.setDaemon(true);
            hilo.start();
        }

        PowerManager.OnThermalStatusChangedListener oyente =
                new PowerManager.OnThermalStatusChangedListener() {
            @Override public void onThermalStatusChanged(int estado) {
                Log.i(TAG, "[thermo] estado " + estado + " " + nombreTermico(estado));
                evaluarEscalon(p, estado, false);
            }
        };
        try {
            pm.addThermalStatusListener(getMainExecutor(), oyente);
            evaluarEscalon(p, inicial, false);
        } catch (RuntimeException e) {
            Log.w(TAG, "sin oyente termico: " + e);
        }
    }

    /** Decide el escalon para ese estado termico (y si el margen de prevision
     * se ha agotado) y lo apunta en las preferencias y en el log. Las subidas
     * valen al momento; la vuelta al limite del usuario espera 60 s de estado
     * limpio (histeresis: que no vuelva a subir por el camino). El limite del
     * usuario ("limite_fps") no se toca nunca: el escalon va en su clave
     * ("thermo_limite_fps") y se borra al despejarse. NO cambia el cvar en
     * vivo, no se puede: nfsmw_ajustes_graficos.cpp lo declara kRequiresRestart
     * y BucleVblank (nfsmw_nativo_sistema.cpp) fija su ritmo al arrancar; lo
     * aplica Ajustes.limiteFps() al montar los argumentos del siguiente
     * arranque. Siempre desde el hilo principal. */
    private void evaluarEscalon(SharedPreferences p, int estado, boolean margenAgotado) {
        int objetivo;
        String porQue;
        if (estado >= 3) {  // severe, critical, emergency, shutdown
            objetivo = 30;
            porQue = nombreTermico(estado);
        } else if (estado == 2 || margenAgotado) {  // moderate, o sin margen aun en light
            objetivo = 40;
            porQue = margenAgotado
                    ? "margen agotado (" + nombreTermico(estado) + ")" : nombreTermico(estado);
        } else {
            objetivo = 0;
            porQue = nombreTermico(estado);
        }
        if (objetivo == escalonTermico) {
            return;  // lo mismo que ya esta pedido: sin ruido
        }
        if (objetivo > 0) {
            if (vueltaTermica != null) {
                relojTermico.removeCallbacks(vueltaTermica);
                vueltaTermica = null;
            }
            escalonTermico = objetivo;
            p.edit().putInt("thermo_limite_fps", objetivo).commit();
            Log.i(TAG, "[thermo] " + porQue + ": pedir " + objetivo
                    + " fps (valdra en el siguiente arranque; el cvar no se cambia en vivo)");
        } else if (vueltaTermica == null) {
            // Histeresis de 60 s: si en ese rato el estado no vuelve a subir, fuera.
            vueltaTermica = new Runnable() { @Override public void run() {
                vueltaTermica = null;
                escalonTermico = 0;
                getSharedPreferences(PREFS_TERMICO, Context.MODE_PRIVATE)
                        .edit().remove("thermo_limite_fps").commit();
                Log.i(TAG, "[thermo] despejado 60 s: limite del usuario en el siguiente arranque");
            }};
            relojTermico.postDelayed(vueltaTermica, 60000);
            Log.i(TAG, "[thermo] " + porQue + ": soltando el escalon en 60 s");
        }
    }

    /** Una propiedad del sistema por reflexion (android.os.SystemProperties): el
     * canal de adb para pruebas sin recompilar. Null si no se puede leer. */
    private static String propiedad(String clave, String porDefecto) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return (String) sp.getMethod("get", String.class, String.class)
                    .invoke(null, clave, porDefecto);
        } catch (Exception e) {
            return null;
        }
    }

    // [power] el consumo de bateria cada segundo: la unica medida de coste que el
    // gobernador de la GPU no puede esconder (con tope de fps baja el reloj hasta
    // que la GPU llena el fotograma). Trasplantado del NhlActivity del NHL4droid.
    private void startPowerLog() {
        final BatteryManager bm = (BatteryManager) getSystemService(BATTERY_SERVICE);
        Thread hilo = new Thread(new Runnable() { @Override public void run() {
            IntentFilter filtro = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
            while (true) {
                long ua = bm.getLongProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW);
                Intent b = registerReceiver(null, filtro);
                int mv = b != null ? b.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0) : 0;
                Log.i(TAG, "[power] " + Math.abs(ua) * mv / 1000000 + " mW ("
                        + ua + " uA, " + mv + " mV)");
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }}, "nfsmw-power");
        hilo.setDaemon(true);
        hilo.start();
    }

    // El modo de pantalla mas cercano a los Hz pedidos, y un setFrameRate en la
    // superficie: vivo ignora el preferredDisplayModeId de la ventana, pero el
    // voto en la capa del juego si lo atiende. Menos Hz de scanout = menos vatios.
    private void pedirModoPantalla(String hzTexto) {
        float quiero;
        try {
            quiero = Float.parseFloat(hzTexto);
        } catch (NumberFormatException e) {
            Log.w(TAG, "debug.nfsmw.refresh mal: " + hzTexto);
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode actual = display.getMode();
        Display.Mode mejor = actual;
        for (Display.Mode modo : display.getSupportedModes()) {
            if (modo.getPhysicalWidth() == actual.getPhysicalWidth()
                    && modo.getPhysicalHeight() == actual.getPhysicalHeight()
                    && Math.abs(modo.getRefreshRate() - quiero)
                        < Math.abs(mejor.getRefreshRate() - quiero)) {
                mejor = modo;
            }
        }
        WindowManager.LayoutParams atributos = getWindow().getAttributes();
        atributos.preferredDisplayModeId = mejor.getModeId();
        getWindow().setAttributes(atributos);
        Log.i(TAG, "modo de pantalla " + mejor.getModeId() + " @ " + mejor.getRefreshRate() + " Hz");
        final float ritmo = mejor.getRefreshRate();
        if (mSurface != null) mSurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                holder.getSurface().setFrameRate(ritmo, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {}
            @Override public void surfaceDestroyed(SurfaceHolder holder) {}
        });
    }

    /** Un boton de mando fisico esconde los controles tactiles. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (touchController != null && TouchControllerView.esFuenteDeMando(event.getSource())) {
            touchController.mandoUsado();
        }
        return super.dispatchKeyEvent(event);
    }

    /** Y tambien los sticks y gatillos, que llegan como movimiento. */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (touchController != null && TouchControllerView.esFuenteDeMando(event.getSource())) {
            touchController.mandoUsado();
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    protected void onPause() {
        if (touchController != null) touchController.clearInput();
        callarVibracion(CALLADA_SEGUNDO_PLANO, true);
        // En segundo plano no hay nadie mirando el rotulo: no despertar al hilo
        // principal dos veces por segundo para nada.
        reloj.removeCallbacksAndMessages(null);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        callarVibracion(CALLADA_SEGUNDO_PLANO, false);
        if (actualizarFps != null) {
            reloj.removeCallbacks(actualizarFps);
            reloj.post(actualizarFps);
        }
    }

    /**
     * Donde y a que tamano se ve el juego. Dos cosas por separado:
     *
     *   la VISTA   el rectangulo de la pantalla que ocupa. Sin estirar, el
     *              16:9 mas grande que cabe, centrado sobre negro; estirando,
     *              la pantalla entera.
     *   el BUFER   los pixeles que se presentan: 720x480, 1280x720, o los de
     *              la vista. El compositor de Android escala el bufer a la
     *              vista gratis, asi que 720x480 en una vista 16:9 queda
     *              anamorfico, como el 480p panoramico de la Xbox 360.
     *
     * La proporcion la pone la vista: el presentador va con
     * present_letterbox=false y llena el bufer entero (ver Ajustes).
     *
     * SDL mete la superficie con WRAP_CONTENT, y con setFixedSize un
     * SurfaceView en WRAP_CONTENT mide lo que el bufer (SurfaceView.onMeasure):
     * la vista encogia al tamano del bufer, en una esquina. Por eso aqui la
     * vista lleva siempre un tamano exacto.
     */
    private void ajustarSuperficie(String resolucion, boolean estirar, boolean panoramico) {
        if (mSurface == null || mLayout == null) {
            return;
        }
        mLayout.setBackgroundColor(Color.BLACK);
        int anchoBufer = Ajustes.ancho(resolucion);
        int altoBufer = Ajustes.alto(resolucion);
        if (anchoBufer > 0) {
            mSurface.getHolder().setFixedSize(anchoBufer, altoBufer);
        } else {
            mSurface.getHolder().setSizeFromLayout();
        }
        // El tamano real de lo que deja Android (recorte de la camara, barras
        // del sistema) solo se sabe al maquetar, y puede cambiar.
        mLayout.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            private int ultimoAncho = -1;
            private int ultimoAlto = -1;

            @Override
            public void onLayoutChange(View v, int l, int t, int r, int b,
                                       int ol, int ot, int or, int ob) {
                int ancho = r - l;
                int alto = b - t;
                if (ancho <= 0 || alto <= 0 || (ancho == ultimoAncho && alto == ultimoAlto)) {
                    return;
                }
                ultimoAncho = ancho;
                ultimoAlto = alto;
                int w = ancho;
                int h = alto;
                if (!estirar) {
                    if (ancho * 9 > alto * 16) {
                        w = alto * 16 / 9;
                    } else {
                        h = ancho * 9 / 16;
                    }
                }
                // Panoramico con un bufer fijo: del alto fijo, y del ancho que da la
                // vista; si no, el juego (ya ancho) se encogeria a 16:9 y se estiraria.
                int anchoPanoramico = panoramico && anchoBufer > 0
                        ? Math.round(altoBufer * (float) w / h) & ~1 : 0;
                RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(w, h);
                lp.addRule(RelativeLayout.CENTER_IN_PARENT);
                // Cambiar la maquetacion desde su propio aviso no se aplica
                // hasta la siguiente pasada: se pide despues.
                v.post(() -> {
                    if (anchoPanoramico > 0) {
                        mSurface.getHolder().setFixedSize(anchoPanoramico, altoBufer);
                    }
                    mSurface.setLayoutParams(lp);
                });
                if (BuildConfig.DEBUG) {
                    Log.i(TAG, "Superficie: vista " + w + "x" + h + ", bufer "
                            + (anchoBufer > 0 ? anchoBufer + "x" + altoBufer : w + "x" + h)
                            + (estirar ? " (estirada)" : " (16:9)"));
                }
            }
        });
    }

    /** Ancho entre alto de la pantalla entera, en horizontal. */
    private double proporcionPantalla() {
        int ancho;
        int alto;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Rect r = getWindowManager().getMaximumWindowMetrics().getBounds();
            ancho = r.width();
            alto = r.height();
        } else {
            Point p = new Point();
            getWindowManager().getDefaultDisplay().getRealSize(p);
            ancho = p.x;
            alto = p.y;
        }
        return Math.max(ancho, alto) / (double) Math.max(1, Math.min(ancho, alto));
    }

    /** Un rotulo en la esquina con los fps del juego, cada medio segundo. */
    private void ponerContadorFps() {
        if (mLayout == null) {
            return;
        }
        textoFps = new TextView(this);
        textoFps.setTextColor(Color.WHITE);
        textoFps.setBackgroundColor(0x99000000);
        textoFps.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        textoFps.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        int m = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 6,
                getResources().getDisplayMetrics());
        textoFps.setPadding(m, m / 2, m, m / 2);
        textoFps.setText("-- fps");
        RelativeLayout.LayoutParams lp = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, RelativeLayout.LayoutParams.WRAP_CONTENT);
        lp.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        lp.addRule(RelativeLayout.ALIGN_PARENT_START);
        lp.setMargins(m * 2, m * 2, 0, 0);
        mLayout.addView(textoFps, lp);
        actualizarFps = new Runnable() {
            // El ultimo texto puesto: setText relanza la maquetacion del rotulo
            // aunque el texto no cambie, y con los fps clavados es lo normal.
            private String ultimo = "";

            @Override
            public void run() {
                float fps, ms;
                try {
                    fps = nativeFps();
                    ms = nativeMsPorFotograma();
                } catch (UnsatisfiedLinkError e) {
                    fps = ms = -1;
                }
                String texto = fps < 0
                        ? "-- fps"
                        : String.format(Locale.ROOT, "%.0f fps  %.1f ms", fps, ms);
                if (!texto.equals(ultimo)) {
                    textoFps.setText(texto);
                    ultimo = texto;
                }
                reloj.postDelayed(this, 500);
            }
        };
        reloj.post(actualizarFps);
    }

    @Override
    protected void onDestroy() {
        if (touchController != null) touchController.disconnect();
        reloj.removeCallbacksAndMessages(null);
        super.onDestroy();
        // Proceso propio: al cerrar la partida, fuera. Asi la siguiente arranca
        // con las librerias nativas sin cargar.
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    /**
     * SDL3 va enlazado ESTATICO dentro de librexruntime.so, y ahi estan sus
     * metodos JNI (nativeSetupJNI y compania). Java solo los encuentra en
     * librerias cargadas con System.loadLibrary, no en las que arrastra el
     * enlazador como dependencia, asi que rexruntime va explicita. La ultima
     * de la lista es la que tiene SDL_main: la libmain de la edicion de la ISO
     * (libmain.so, o libmain_usa.so... en un APK con varias; ver Ediciones).
     */
    @Override
    protected String[] getLibraries() {
        // Con el motor nativo SDL3 va en su propia libreria, como espera SDLActivity.
        return MotorNativo.ACTIVO
                ? new String[] {"SDL3", edicion.libreria}
                : new String[] {"rexruntime", edicion.libreria};
    }

    /**
     * SDL pide una orientacion segun el tamano de su ventana al crearla, y con
     * un bufer fijo (720x480, 1280x720) o al volver de segundo plano le puede
     * salir vertical. El juego va siempre apaisado, hacia el lado que se gire
     * el movil. La pantalla de inicio no: esa gira libremente.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected String[] getArguments() {
        return argumentos;
    }

    /**
     * Puente que busca el parche Android del SDK (android_runtime.cpp) por esta
     * firma exacta en la clase de la actividad, para abrir URIs content:// desde
     * codigo nativo. Devuelve null si algo falla.
     */
    // --- La vibracion del mando tactil (Ajustes.vibracion).
    private static volatile boolean vibracion;
    private static Vibrator vibrador;
    // Lo ultimo que pidio el juego (0..255), lo que esta puesto y por que se
    // calla (en segundo plano, editando el mando): sin perder lo pedido, para
    // volver a vibrar al acabar.
    private static int vibracionPedida;
    private static int vibracionPuesta;
    private static int vibracionCallada;
    static final int CALLADA_SEGUNDO_PLANO = 1;
    static final int CALLADA_EDITOR = 2;
    // Hay un mando fisico (USB o Bluetooth): vibra el, no el movil.
    static final int CALLADA_MANDO = 4;

    /**
     * El mando tactil vibra: lo pide el juego desde su hilo (nativo_android.cpp),
     * con una fuerza de 0 a 255; 0 la para. Dura hasta que el juego la cambie,
     * como en el mando de la Xbox 360.
     */
    public static synchronized void vibrarMando(int amplitud) {
        vibracionPedida = amplitud;
        aplicarVibracion();
    }

    /** Calla la vibracion por un motivo (CALLADA_*) mientras dure. */
    static synchronized void callarVibracion(int motivo, boolean callar) {
        vibracionCallada = callar ? vibracionCallada | motivo : vibracionCallada & ~motivo;
        aplicarVibracion();
    }

    private static void aplicarVibracion() {
        Vibrator v = vibrador;
        if (v == null) return;
        int a = vibracion && vibracionCallada == 0 ? Math.min(255, vibracionPedida) : 0;
        if (a == vibracionPuesta) return;
        vibracionPuesta = a;
        if (a <= 0) {
            v.cancel();
        } else {
            // Una forma de onda que se repite: sigue hasta la siguiente llamada.
            v.vibrate(VibrationEffect.createWaveform(new long[] {1000}, new int[] {a}, 0));
        }
    }

    public static ParcelFileDescriptor openContentFd(String uri, String mode) {
        SDLActivity yo = mSingleton;
        if (yo == null || uri == null) {
            return null;
        }
        try {
            return yo.getContentResolver()
                    .openFileDescriptor(Uri.parse(uri), mode == null ? "r" : mode);
        } catch (Exception e) {
            return null;
        }
    }
}
