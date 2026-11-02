package ai.zetic.lfm3demo

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors

/** Six fixed SDK examples: three input types, each with text or speech output. */
class MainActivity : Activity() {
    private val tag = "LFM3Demo"
    private val ui = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()
    private val samples = listOf(
        Example("summary", "Text → text", "Long article", "Summary"),
        Example("tts", "Text → audio", "Text prompt", "Spoken answer"),
        Example("image", "Image → text", "Image + prompt", "Text description"),
        Example("vl_audio", "Image → audio", "Image + prompt", "Spoken description"),
        Example("asr", "Audio → text", "WAV + prompt", "Transcript"),
        Example("voice", "Audio → audio", "WAV question", "Spoken answer"),
    )
    private data class Example(val scenario: String, val title: String, val input: String, val output: String)

    private lateinit var status: TextView
    private lateinit var inputView: TextView
    private lateinit var inputImageView: ImageView
    private lateinit var inputAudioButton: Button
    private lateinit var outputView: TextView
    private lateinit var metricsView: TextView
    private lateinit var playButton: Button
    private lateinit var stopButton: Button
    private lateinit var buttons: List<Button>
    private var llm: HexagonLlm? = null
    private var loading = false
    private var busy = false
    private var hookRunning = false
    private var pending: String? = null
    private var resultAudio: File? = null
    private var inputAudio: Media.Pcm? = null
    private var player: StreamPlayer? = null
    private val resultsDir get() = File(getExternalFilesDir(null), "results")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        handleHook(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleHook(intent)
        if (llm != null) startPending() else if (!loading) loadModel()
    }

    private fun handleHook(intent: Intent) {
        intent.getStringExtra("scenario")?.let { name ->
            if (name == "all" || samples.any { it.scenario == name }) {
                pending = name
                hookRunning = true
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        if (llm == null && !loading) loadModel() else startPending()
    }

    override fun onStop() {
        super.onStop()
        player?.stop(); player = null
        if (hookRunning) return
        llm?.cancel()
        worker.execute {
            llm?.close(); llm = null
            ui.post { status.text = "Model unloaded"; updateButtons() }
        }
    }

    private fun modelDir(): File? {
        val root = File(getExternalFilesDir(null), "models")
        val targets = root.walkTopDown().maxDepth(4).filter { it.isFile && it.name == "manifest.json" }.toList()
        return targets.mapNotNull { it.parentFile?.parentFile }
            .sortedByDescending { it.name == "lfm3-omni-2b" }.firstOrNull()
    }

    private fun loadModel() {
        val dir = modelDir()
        if (dir == null) {
            status.text = "Model not found. See README for the download and adb push steps."
            return
        }
        loading = true; status.text = "Loading ${dir.name}…"; updateButtons()
        worker.execute {
            try {
                val loaded = HexagonLlm.load(dir.absolutePath, applicationInfo.nativeLibraryDir, LlmOptions(maxContext = 2048))
                llm = loaded
                ui.post {
                    loading = false
                    status.text = "${loaded.info.modelId} · ${loaded.info.soc} · context 2048"
                    Log.i(tag, "LOADED ${loaded.info}")
                    updateButtons(); startPending()
                }
            } catch (e: Exception) {
                Log.e(tag, "LOAD FAILED", e)
                ui.post { loading = false; status.text = "Load failed: ${e.message}"; updateButtons() }
            }
        }
    }

    private fun runExample(name: String) {
        if (busy) return
        if (llm == null) { pending = name; loadModel(); return }
        runNames(listOf(name), false)
    }

    private fun startPending() {
        val name = pending ?: return
        if (busy || llm == null) return
        pending = null
        runNames(if (name == "all") samples.map { it.scenario } else listOf(name), true)
    }

    private fun runNames(names: List<String>, hook: Boolean) {
        busy = true; updateButtons()
        worker.execute {
            for (name in names) runOne(name)
            Log.i(tag, "HOOK DONE ${if (hook) names.joinToString(",") else names.single()}")
            ui.post { busy = false; hookRunning = false; updateButtons() }
        }
    }

    private fun runOne(name: String) {
        val example = samples.first { it.scenario == name }
        val scenario = Scenarios.load(assets, name)
        val turn = scenario.turns.first()
        ui.post {
            player?.stop(); player = null; resultAudio = null
            inputAudio = null
            inputView.text = buildString {
                append("${example.title}\n${example.input}: ")
                append(turn.text.ifEmpty { "(no text prompt)" })
                turn.imageFile?.let { append("\nImage: $it") }
                turn.audioFile?.let { append("\nAudio: $it") }
            }
            outputView.text = "Running…"
            metricsView.text = "TTFT —   ·   TPS —"
            playButton.visibility = View.GONE
            inputImageView.visibility = View.GONE
            inputAudioButton.visibility = View.GONE
        }
        val log = StringBuilder("scenario $name\n")
        try {
            val m = llm ?: error("model unloaded")
            m.reset()
            val image = turn.imageFile?.let { path -> assets.open(path).use { Media.parsePpm(it.readBytes()) } }
            val audio = turn.audioFile?.let { path -> assets.open(path).use { Media.readWav(it.readBytes()) } }
            image?.let { (rgb, w, h) -> m.addImage(rgb, w, h) }
            audio?.let { m.addAudio(it.samples, it.rate, it.channels) }
            ui.post {
                image?.let { (rgb, w, h) ->
                    inputImageView.setImageBitmap(Media.bitmapOfRgb(rgb, w, h))
                    inputImageView.visibility = View.VISIBLE
                }
                audio?.let {
                    inputAudio = it
                    inputAudioButton.text = "Play input audio · %.1f s".format(it.seconds)
                    inputAudioButton.visibility = View.VISIBLE
                }
            }
            if (turn.text.isNotEmpty()) m.addText(turn.text)
            val speech = turn.out != "text"
            val response = m.chat(Generation(maxTokens = 256, seed = 1, textOut = turn.out != "audio", audioOut = speech))
            val file = if (response.audio.isNotEmpty()) File(resultsDir, "$name.wav").also { Media.writeWav(it, response.audio, response.audioRate) } else null
            val output = buildString {
                append(response.text.ifBlank { if (file != null) "Audio output" else "(empty response)" })
                if (file != null) append("\nAudio: %.1f s".format(response.audioSeconds))
            }
            log.append("TTFT ${response.ttftMs} ms\nTPS ${response.tokensPerSecond}\ntext: ${response.text}\n")
            file?.let { log.append("audio: ${it.name} ${response.audio.size} samples\n") }
            log.append("result: OK\n")
            ui.post {
                outputView.text = output
                metricsView.text = "TTFT %.0f ms   ·   TPS %.1f".format(response.ttftMs, response.tokensPerSecond)
                resultAudio = file
                playButton.visibility = if (file != null) View.VISIBLE else View.GONE
                status.text = "${example.output} · context 2048 · ${response.finish}"
            }
            Log.i(tag, "SCENARIO $name OK")
        } catch (e: Exception) {
            log.append("result: FAIL ${e.message}\n")
            ui.post { outputView.text = "Error: ${e.message}"; metricsView.text = "TTFT —   ·   TPS —"; status.text = "Sample failed" }
            Log.e(tag, "SCENARIO $name FAIL", e)
        }
        resultsDir.mkdirs()
        File(resultsDir, "$name.txt").writeText(log.toString())
    }

    private fun updateButtons() {
        val ready = llm != null && !busy && !loading
        if (::buttons.isInitialized) buttons.forEach { it.isEnabled = ready }
        if (::stopButton.isInitialized) stopButton.isEnabled = busy || player != null
    }

    private fun buildUi() {
        val density = resources.displayMetrics.density
        fun dp(x: Int) = (x * density).toInt()
        val dark = resources.configuration.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK == android.content.res.Configuration.UI_MODE_NIGHT_YES
        val foreground = if (dark) Color.WHITE else Color.BLACK
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(20), dp(20), dp(20))
            setBackgroundColor(if (dark) 0xff171717.toInt() else Color.WHITE)
        }
        val scroll = ScrollView(this)
        val body = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        body.addView(TextView(this).apply { text = getString(R.string.app_name); textSize = 26f; setTextColor(foreground) })
        status = TextView(this).apply { text = "Looking for model…"; textSize = 14f; setPadding(0, dp(8), 0, dp(18)) }
        body.addView(status)
        buttons = samples.chunked(2).flatMap { pair ->
            val row = LinearLayout(this)
            val rowButtons = pair.map { sample ->
                Button(this).apply {
                    text = sample.title; isAllCaps = false
                    setOnClickListener { runExample(sample.scenario) }
                    row.addView(this, LinearLayout.LayoutParams(0, dp(64), 1f))
                }
            }
            body.addView(row)
            rowButtons
        }
        inputView = TextView(this).apply { text = "Choose one of the six samples."; textSize = 16f; setTextColor(foreground); setPadding(0, dp(20), 0, dp(12)) }
        inputImageView = ImageView(this).apply { visibility = View.GONE; adjustViewBounds = true; scaleType = ImageView.ScaleType.FIT_START }
        inputAudioButton = Button(this).apply {
            text = "Play input audio"; isAllCaps = false; visibility = View.GONE
            setOnClickListener {
                inputAudio?.let { pcm ->
                    player?.stop()
                    val mono = if (pcm.channels == 1) pcm.samples else FloatArray(pcm.samples.size / pcm.channels) { i ->
                        (0 until pcm.channels).sumOf { c -> pcm.samples[i * pcm.channels + c].toDouble() }.toFloat() / pcm.channels
                    }
                    player = StreamPlayer(pcm.rate).also { it.write(mono); it.finish() }
                    updateButtons()
                }
            }
        }
        outputView = TextView(this).apply { text = "Output appears here."; textSize = 18f; setTextColor(foreground); setTextIsSelectable(true); setPadding(0, dp(8), 0, dp(12)) }
        metricsView = TextView(this).apply { text = "TTFT —   ·   TPS —"; textSize = 16f; setTextColor(foreground) }
        body.addView(inputView)
        body.addView(inputImageView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(190)))
        body.addView(inputAudioButton)
        body.addView(TextView(this).apply { text = "Output"; textSize = 14f; setTextColor(foreground) })
        body.addView(outputView)
        body.addView(metricsView)
        val controls = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        playButton = Button(this).apply {
            text = "Play audio"; isAllCaps = false; visibility = View.GONE
            setOnClickListener {
                resultAudio?.let { file ->
                    player?.stop()
                    val pcm = Media.readWav(file.readBytes())
                    player = StreamPlayer(pcm.rate).also { it.write(pcm.samples); it.finish() }
                    updateButtons()
                }
            }
        }
        stopButton = Button(this).apply {
            text = "Stop"; isAllCaps = false
            setOnClickListener { player?.stop(); player = null; llm?.cancel(); updateButtons() }
        }
        controls.addView(playButton)
        controls.addView(stopButton)
        body.addView(controls)
        scroll.addView(body)
        root.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(root)
        updateButtons()
    }
}
