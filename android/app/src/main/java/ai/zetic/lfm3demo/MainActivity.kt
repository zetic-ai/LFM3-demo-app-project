package ai.zetic.lfm3demo

import android.app.Activity
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.os.Build
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

/** Six LFM3 generation examples and three D1 decision examples. */
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
    private val d1Names = listOf("d1_text", "d1_image", "d1_audio")

    private lateinit var status: TextView
    private lateinit var inputView: TextView
    private lateinit var inputImageView: ImageView
    private lateinit var inputAudioButton: Button
    private lateinit var outputView: TextView
    private lateinit var metricsView: TextView
    private lateinit var playButton: Button
    private lateinit var stopButton: Button
    private lateinit var buttons: List<Button>
    private lateinit var lfmPrepareButton: Button
    private lateinit var d1Buttons: List<Button>
    private lateinit var d1PrepareButtons: List<Button>
    private var llm: HexagonLlm? = null
    private var preparedD1: String? = null
    private var loading = false
    private var busy = false
    private var hookRunning = false
    private var pending: String? = null
    private var resultAudio: File? = null
    private var inputAudio: Media.Pcm? = null
    private var player: StreamPlayer? = null
    private var started = false
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
        if (pending in d1Names || llm != null) startPending() else if (!loading) loadModel()
    }

    private fun handleHook(intent: Intent) {
        intent.getStringExtra("scenario")?.let { name ->
            if (name == "all" || samples.any { it.scenario == name } || name in d1Names) {
                pending = name
                hookRunning = true
                window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
    }

    override fun onStart() {
        super.onStart()
        started = true
        if (pending in d1Names) startPending()
        else if (pending != null && llm == null && !loading) loadModel()
        else if (pending != null) startPending()
        else if (llm == null && preparedD1 == null && !loading && !busy) {
            if (d1Dir("d1_image") != null) prepareD1("d1_image") else loadModel()
        }
    }

    override fun onStop() {
        super.onStop()
        started = false
        player?.stop(); player = null
        if (hookRunning) return
        llm?.cancel()
        Native.decisionCancel()
        worker.execute {
            llm?.close(); llm = null
            Native.decisionUnload(); preparedD1 = null
            ui.post { status.text = "Model unloaded"; updateButtons() }
        }
    }

    private fun modelDir(): File? {
        val root = File(getExternalFilesDir(null), "models")
        val targets = root.walkTopDown().maxDepth(4).filter { it.isFile && it.name == "manifest.json" }.toList()
        return targets.mapNotNull { it.parentFile?.parentFile }
            .firstOrNull { it.name == "lfm3-omni-2b" }
    }

    private fun d1Dir(name: String): File? {
        val model = if (name == "d1_text") "d1-2b" else "d1-omni-2b"
        val dir = File(getExternalFilesDir(null), "models/qualcomm/$model")
        val soc = Build.SOC_MODEL.uppercase()
        val target = when {
            "SM8750" in soc -> "v79"
            "SM8850" in soc -> "v81"
            else -> return null
        }
        return dir.takeIf { it.isDirectory && File(it, "$target/manifest.json").isFile }
    }

    private fun loadModel() {
        if (llm != null) {
            status.text = "${llm?.info?.modelId} ready · context 2048"
            return
        }
        val dir = modelDir()
        if (dir == null) {
            status.text = "Model not found. See README for the download and adb push steps."
            return
        }
        loading = true; status.text = "Loading ${dir.name}…"; updateButtons()
        worker.execute {
            try {
                Native.decisionUnload(); preparedD1 = null
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
        if (busy || (llm == null && name !in d1Names)) return
        pending = null
        if (name in d1Names) runD1Example(name)
        else runNames(if (name == "all") samples.map { it.scenario } else listOf(name), true)
    }

    private fun runD1Example(name: String) {
        val dir = d1Dir(name)
        if (dir == null) {
            status.text = "D1 model missing for this device (see README)."
            return
        }
        if (busy) return
        busy = true; updateButtons()
        worker.execute {
            val log = StringBuilder("scenario $name\n")
            try {
                val loadMs = ensureD1Loaded(dir)
                val isAudio = name == "d1_audio"
                val isImage = name == "d1_image"
                val state = if (isAudio || isImage) "" else assets.open("d1_text/ticket.txt").bufferedReader().use { it.readText().trim() }
                val audio = if (isAudio) assets.open("asr/jfk.wav").use { Media.readWav(it.readBytes()) } else null
                val image = if (isImage) assets.open("d1_image/cats.ppm").use { Media.parsePpm(it.readBytes()) } else null
                ui.post {
                    player?.stop(); player = null; resultAudio = null
                    inputView.text = when {
                        isAudio -> "D1-Omni-2B · natural speech\nAudio: asr/jfk.wav"
                        isImage -> "D1-Omni-2B · two cats\nImage: d1_image/cats.ppm"
                        else -> "D1-2B · ticket\n$state"
                    }
                    inputImageView.visibility = if (image != null) View.VISIBLE else View.GONE
                    image?.let { (rgb, w, h) -> inputImageView.setImageBitmap(Media.bitmapOfRgb(rgb, w, h)) }
                    inputAudio = audio
                    inputAudioButton.visibility = if (audio != null) View.VISIBLE else View.GONE
                    if (audio != null) inputAudioButton.text = "Play input audio · %.1f s".format(audio.seconds)
                    playButton.visibility = View.GONE
                    outputView.text = "Running D1 decision…"
                    metricsView.text = "Decision —"
                    status.text = if (loadMs > 0) "${dir.name} loaded in %.0f ms".format(loadMs) else "${dir.name} ready"
                }
                val result = Native.decision(dir.absolutePath, applicationInfo.nativeLibraryDir, name,
                    state, audio?.samples, audio?.rate ?: 0, audio?.channels ?: 0,
                    image?.first, image?.second ?: 0, image?.third ?: 0)
                val parts = result.split("|", limit = 4)
                if (parts[0] != "Ok" || parts.size != 4) throw errorFrom(result)
                val ms = parts[1].toDoubleOrNull() ?: 0.0
                val passes = parts[2].toIntOrNull() ?: 0
                log.append("load_ms $loadMs\ndecision_ms $ms\npasses $passes\nanswers:\n${parts[3]}result: OK\n")
                ui.post {
                    outputView.text = parts[3].trim()
                    metricsView.text = "Load %.0f ms   ·   Decision %.1f ms   ·   %d pass(es)".format(loadMs, ms, passes)
                    status.text = "${dir.name} · decision complete"
                }
                Log.i(tag, "SCENARIO $name OK")
            } catch (e: Exception) {
                log.append("result: FAIL ${e.message}\n")
                ui.post { outputView.text = "Error: ${e.message}"; status.text = "D1 sample failed" }
                Log.e(tag, "SCENARIO $name FAIL", e)
            } finally {
                resultsDir.mkdirs()
                File(resultsDir, "$name.txt").writeText(log.toString())
                ui.post {
                    busy = false; hookRunning = pending != null; updateButtons(); startPending()
                }
            }
        }
    }

    /** SDK supports one active NPU model. Keep the selected D1 model loaded until another model is chosen. */
    private fun ensureD1Loaded(dir: File): Double {
        if (preparedD1 == dir.absolutePath) return 0.0
        llm?.close(); llm = null
        preparedD1 = null
        val start = System.nanoTime()
        val result = Native.decisionLoad(dir.absolutePath, applicationInfo.nativeLibraryDir)
        if (!result.startsWith("Ok|")) throw errorFrom(result)
        preparedD1 = dir.absolutePath
        return (System.nanoTime() - start) / 1_000_000.0
    }

    private fun prepareD1(name: String) {
        val dir = d1Dir(name) ?: return
        if (busy || loading) return
        busy = true; status.text = "Preparing ${dir.name}…"; updateButtons()
        worker.execute {
            try {
                val ms = ensureD1Loaded(dir)
                ui.post {
                    status.text = "${dir.name} ready"
                    metricsView.text = "Load %.0f ms   ·   ready for D1 examples".format(ms)
                }
            } catch (e: Exception) {
                Log.e(tag, "D1 PREPARE FAIL", e)
                ui.post { status.text = "D1 prepare failed: ${e.message}" }
            } finally {
                ui.post { busy = false; updateButtons(); startPending() }
            }
        }
    }

    private fun runNames(names: List<String>, hook: Boolean) {
        busy = true; updateButtons()
        worker.execute {
            for (name in names) runOne(name)
            Log.i(tag, "HOOK DONE ${if (hook) names.joinToString(",") else names.single()}")
            ui.post { busy = false; hookRunning = pending != null; updateButtons(); startPending() }
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
        val ready = modelDir() != null && !busy && !loading
        if (::buttons.isInitialized) buttons.forEach { it.isEnabled = ready }
        if (::lfmPrepareButton.isInitialized) lfmPrepareButton.isEnabled = ready
        if (::d1Buttons.isInitialized) d1Buttons.forEachIndexed { i, button ->
            button.isEnabled = !busy && !loading && d1Dir(d1Names[i]) != null
        }
        if (::d1PrepareButtons.isInitialized) d1PrepareButtons.forEachIndexed { i, button ->
            button.isEnabled = !busy && !loading && d1Dir(if (i == 0) "d1_text" else "d1_image") != null
        }
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
        val header = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        header.addView(ImageView(this).apply {
            setImageResource(R.drawable.zetic_brand_mark)
            contentDescription = "Zetic logo"
        }, LinearLayout.LayoutParams(dp(40), dp(40)).apply { marginEnd = dp(12) })
        header.addView(TextView(this).apply {
            text = getString(R.string.app_name)
            textSize = 22f
            setTextColor(foreground)
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        body.addView(header)
        status = TextView(this).apply { text = "Looking for model…"; textSize = 14f; setPadding(0, dp(8), 0, dp(18)) }
        body.addView(status)
        lfmPrepareButton = Button(this).apply {
            text = "Prepare LFM3 Omni"; isAllCaps = false
            setOnClickListener { loadModel() }
        }
        body.addView(lfmPrepareButton)
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
        body.addView(TextView(this).apply {
            text = "D1 decisions (device model required)"; textSize = 18f; setTextColor(foreground)
            setPadding(0, dp(16), 0, dp(4))
        })
        val prepareRow = LinearLayout(this)
        d1PrepareButtons = listOf("Prepare D1-2B" to "d1_text", "Prepare D1-Omni" to "d1_image").map { (label, name) ->
            Button(this).apply {
                text = label; isAllCaps = false
                setOnClickListener { prepareD1(name) }
                prepareRow.addView(this, LinearLayout.LayoutParams(0, dp(56), 1f))
            }
        }
        body.addView(prepareRow)
        d1Buttons = listOf("D1-2B · ticket" to "d1_text", "D1-Omni · image" to "d1_image",
            "D1-Omni · audio" to "d1_audio").chunked(2).flatMap { pair ->
            val row = LinearLayout(this)
            val rowButtons = pair.map { (label, name) ->
                Button(this).apply {
                    text = label; isAllCaps = false
                    setOnClickListener { runD1Example(name) }
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
            setOnClickListener { player?.stop(); player = null; llm?.cancel(); Native.decisionCancel(); updateButtons() }
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
