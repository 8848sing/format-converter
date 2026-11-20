package com.formatconverter.app;

import android.Manifest;
import android.content.ClipData;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Shader;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.checkbox.MaterialCheckBox;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.slider.Slider;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;

/**
 * 格式转换工具 Android 前端。
 *
 * <ul>
 *   <li>转换全部交给 C++ 核心（libfccore_jni.so），Java 只做选择文件、队列、进度和输出；</li>
 *   <li>事件流式回调：每完成一个文件就刷新进度和状态；</li>
 *   <li>支持「取消」，输出自动发布到系统相册/下载目录，并可直接分享；</li>
 *   <li>支持从其它应用分享文件进来（ACTION_SEND / ACTION_VIEW）；</li>
 *   <li>内置设备自检，装完就能确认原生库和解码链是否正常。</li>
 * </ul>
 *
 * 界面上用 Material 3 的「类别 → 操作 → 目标格式」三级胶囊选择，避免一屏四五个下拉框。
 */
public class MainActivity extends AppCompatActivity {

    private static final int REQUEST_FILES = 1001;
    private static final int REQUEST_WRITE_STORAGE = 1002;
    private static final int REQUEST_NOTIFICATIONS = 1003;

    /** Android 端能编码的图片目标格式（TIFF/HEIC/AVIF 只能解码，不能编码）。 */
    private static final Set<String> ANDROID_IMAGE_TARGETS = new HashSet<>(Arrays.asList(
            "png", "jpg", "jpeg", "webp", "bmp", "gif", "tga", "ppm", "ico"));

    private static final String[] DPI_CHOICES = {"100", "150", "200", "300"};
    private static final String PREFS = "fc_prefs";
    private static final String KEY_OP = "last_op";
    private static final String KEY_TARGET = "last_target";

    private final List<OpSpec> specs = new ArrayList<>();
    private final List<String> categories = new ArrayList<>();
    private final List<Entry> entries = new ArrayList<>();
    private final List<File> pendingOutputs = new ArrayList<>();
    private final List<Uri> publishedUris = new ArrayList<>();
    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler ui = new Handler(Looper.getMainLooper());

    private ChipGroup chipCategory;
    private ChipGroup chipOp;
    private ChipGroup chipTarget;
    private ChipGroup chipDpi;
    private Slider sliderQuality;
    private TextView txtQualityValue;
    private TextView txtQueueCount;
    private TextView txtOutput;
    private TextView txtLog;
    private TextView txtStatus;
    private TextView txtVersion;
    private TextView labelTarget;
    private View groupQuality;
    private View groupDpi;
    private ProgressBar progress;
    private ScrollView logPanel;
    private TextView txtEmptyQueue;
    private MaterialButton btnRun;
    private MaterialButton btnCancel;
    private MaterialButton btnShare;
    private MaterialButton btnPick;
    private MaterialButton btnSelfTest;
    private MaterialButton btnClear;
    private MaterialButton btnToggleLog;
    private MaterialCheckBox chkPublish;
    private ListView listFiles;

    private EntryAdapter adapter;
    private SharedPreferences prefs;
    private File outputDir;
    private String baseDir;

    private OpSpec currentSpec;
    private String currentTarget = "";
    private String currentDpi = "200";

    /** 程序化设置 chip 选中状态时抑制回调。 */
    private boolean updating;

    private volatile boolean running;
    private int totalTasks;
    private int doneTasks;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        bindViews();

        baseDir = getFilesDir().getAbsolutePath();
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        File external = getExternalFilesDir(null);
        outputDir = new File(external != null ? external : getFilesDir(), "output");
        ensureDirectory(outputDir);
        txtOutput.setText(getString(R.string.output_dir) + outputDir.getAbsolutePath());

        adapter = new EntryAdapter();
        listFiles.setAdapter(adapter);
        updateFileSummary();

        sliderQuality.addOnChangeListener((slider, value, fromUser) ->
                txtQualityValue.setText(String.valueOf((int) value)));
        txtQualityValue.setText(String.valueOf((int) sliderQuality.getValue()));

        buildDpiChips();

        loadRegistry();

        try {
            String full = NativeCore.version();
            int vIndex = full.indexOf(" v");
            String version = vIndex > 0 ? full.substring(vIndex + 1) : full;
            txtVersion.setText(OpLabels.isEnglish() ? getString(R.string.version_core, version) : version);
        } catch (Throwable exc) {
            txtVersion.setText("核心不可用");
            appendLog("原生库加载失败：" + exc);
        }

        handleIntent(getIntent());

        txtVersion.setOnClickListener(v ->
                startActivity(new Intent(MainActivity.this, AboutActivity.class)));

        if (FmtApplication.hasCrash(this)) {
            appendLog(getString(R.string.crash_found_hint));
        }
        requestNotificationPermissionIfNeeded();
    }

    private void requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},
                    REQUEST_NOTIFICATIONS);
        }
    }

    private void bindViews() {
        chipCategory = findViewById(R.id.chipCategory);
        chipOp = findViewById(R.id.chipOp);
        chipTarget = findViewById(R.id.chipTarget);
        chipDpi = findViewById(R.id.chipDpi);
        sliderQuality = findViewById(R.id.sliderQuality);
        txtQualityValue = findViewById(R.id.txtQualityValue);
        txtQueueCount = findViewById(R.id.txtQueueCount);
        txtOutput = findViewById(R.id.txtOutput);
        txtLog = findViewById(R.id.txtLog);
        txtStatus = findViewById(R.id.txtStatus);
        txtVersion = findViewById(R.id.txtVersion);
        labelTarget = findViewById(R.id.labelTarget);
        groupQuality = findViewById(R.id.groupQuality);
        groupDpi = findViewById(R.id.groupDpi);
        progress = findViewById(R.id.progress);
        logPanel = findViewById(R.id.logPanel);
        txtEmptyQueue = findViewById(R.id.txtEmptyQueue);
        btnRun = findViewById(R.id.btnRun);
        btnCancel = findViewById(R.id.btnCancel);
        btnShare = findViewById(R.id.btnShare);
        btnPick = findViewById(R.id.btnPick);
        btnSelfTest = findViewById(R.id.btnSelfTest);
        btnClear = findViewById(R.id.btnClear);
        btnToggleLog = findViewById(R.id.btnToggleLog);
        chkPublish = findViewById(R.id.chkPublish);
        listFiles = findViewById(R.id.listFiles);

        chipOp.setSingleSelection(true);
        chipOp.setSelectionRequired(true);

        btnPick.setOnClickListener(v -> pickFiles());
        btnClear.setOnClickListener(v -> clearEntries());
        btnSelfTest.setOnClickListener(v -> startSelfTest());
        btnRun.setOnClickListener(v -> startConversion());
        btnCancel.setOnClickListener(v -> cancelJob());
        btnShare.setOnClickListener(v -> shareOutputs());
        btnToggleLog.setOnClickListener(v -> toggleLogPanel());

        listFiles.setOnItemLongClickListener((parent, view, position, id) -> {
            if (running) {
                toast(getString(R.string.toast_busy_queue));
                return true;
            }
            if (position >= 0 && position < entries.size()) {
                entries.remove(position);
                adapter.notifyDataSetChanged();
                updateFileSummary();
            }
            return true;
        });
    }

    // ------------------------------------------------------------------
    // 注册表与选择器（类别 → 操作 → 目标格式）
    // ------------------------------------------------------------------

    private void loadRegistry() {
        String json;
        try {
            json = NativeCore.registryJson();
        } catch (Throwable exc) {
            appendLog("无法读取转换注册表：" + exc);
            return;
        }
        try {
            JSONArray array = new JSONArray(json);
            for (int i = 0; i < array.length(); i++) {
                JSONObject item = array.getJSONObject(i);
                if (!isSupported(item)) {
                    continue;
                }
                String id = item.optString("id");
                boolean imageTargets = "image_convert".equals(id) || "pdf_to_image".equals(id);
                boolean showQuality = "image_convert".equals(id);
                List<String> targets = new ArrayList<>();
                JSONArray accepts = item.optJSONArray("accepts");
                if (accepts != null) {
                    for (int j = 0; j < accepts.length(); j++) {
                        String target = accepts.optString(j);
                        if (imageTargets && !ANDROID_IMAGE_TARGETS.contains(target)) {
                            continue;
                        }
                        targets.add(target);
                    }
                }
                List<String> extensions = new ArrayList<>();
                JSONArray ext = item.optJSONArray("extensions");
                if (ext != null) {
                    for (int j = 0; j < ext.length(); j++) {
                        extensions.add(ext.optString(j));
                    }
                }
                specs.add(new OpSpec(id, item.optString("label", id), item.optString("src"),
                        item.optString("dst"), targets, extensions, imageTargets, showQuality));
            }
        } catch (Exception exc) {
            appendLog("解析转换注册表失败：" + exc);
        }

        addAndroidOnlySpecs();

        categories.clear();
        for (OpSpec spec : specs) {
            String category = categoryOf(spec.src);
            if (!categories.contains(category)) {
                categories.add(category);
            }
        }

        updating = true;
        chipCategory.removeAllViews();
        for (String category : categories) {
            Chip chip = buildChip(OpLabels.category(category));
            chip.setTextColor(ContextCompat.getColorStateList(this, R.color.chip_category_text));
            chip.setChipBackgroundColor(
                    ContextCompat.getColorStateList(this, R.color.chip_category_background));
            chip.setTag(category);
            chipCategory.addView(chip);
        }
        updating = false;

        chipCategory.setOnCheckedStateChangeListener((group, checkedIds) -> {
            if (updating || checkedIds.isEmpty()) {
                return;
            }
            View chip = group.findViewById(checkedIds.get(0));
            if (chip != null) {
                showOpsFor(String.valueOf(chip.getTag()));
            }
        });

        chipTarget.setOnCheckedStateChangeListener((group, checkedIds) -> {
            if (updating || checkedIds.isEmpty()) {
                return;
            }
            View chip = group.findViewById(checkedIds.get(0));
            if (chip != null) {
                currentTarget = String.valueOf(chip.getTag());
            }
        });

        if (!categories.isEmpty()) {
            String first = categories.get(0);
            // 恢复上次使用的转换方式所在的类别。
            String lastOp = prefs == null ? null : prefs.getString(KEY_OP, null);
            for (OpSpec spec : specs) {
                if (spec.id.equals(lastOp)) {
                    first = categoryOf(spec.src);
                    break;
                }
            }
            updating = true;
            checkChipWithTag(chipCategory, first);
            updating = false;
            showOpsFor(first);
        }
    }

    private void buildDpiChips() {
        updating = true;
        chipDpi.removeAllViews();
        for (String dpi : DPI_CHOICES) {
            Chip chip = buildChip(dpi);
            chip.setTag(dpi);
            chipDpi.addView(chip);
        }
        checkChipWithTag(chipDpi, currentDpi);
        updating = false;
        chipDpi.setOnCheckedStateChangeListener((group, checkedIds) -> {
            if (updating || checkedIds.isEmpty()) {
                return;
            }
            View chip = group.findViewById(checkedIds.get(0));
            if (chip != null) {
                currentDpi = String.valueOf(chip.getTag());
            }
        });
    }

    private void showOpsFor(String category) {
        List<OpSpec> ops = new ArrayList<>();
        for (OpSpec spec : specs) {
            if (categoryOf(spec.src).equals(category)) {
                ops.add(spec);
            }
        }
        updating = true;
        chipOp.removeAllViews();
        for (OpSpec spec : ops) {
            Chip chip = buildChip(OpLabels.name(spec.id, shortLabel(spec.label)));
            chip.setTag(spec.id);
            chipOp.addView(chip);
        }
        updating = false;

        if (ops.isEmpty()) {
            currentSpec = null;
            populateTargets(null);
            return;
        }
        OpSpec pick = ops.get(0);
        String lastOp = prefs == null ? null : prefs.getString(KEY_OP, null);
        for (OpSpec spec : ops) {
            if (spec.id.equals(lastOp)) {
                pick = spec;
                break;
            }
        }
        updating = true;
        checkChipWithTag(chipOp, pick.id);
        updating = false;

        chipOp.setOnCheckedStateChangeListener((group, checkedIds) -> {
            if (updating || checkedIds.isEmpty()) {
                return;
            }
            View chip = group.findViewById(checkedIds.get(0));
            if (chip != null) {
                applySpec(findSpec(String.valueOf(chip.getTag())));
            }
        });

        applySpec(pick);
    }

    private void applySpec(OpSpec spec) {
        currentSpec = spec;
        if (spec == null) {
            groupQuality.setVisibility(View.GONE);
            groupDpi.setVisibility(View.GONE);
            populateTargets(null);
            return;
        }
        groupQuality.setVisibility(spec.showQuality ? View.VISIBLE : View.GONE);
        groupDpi.setVisibility("pdf_to_image".equals(spec.id) ? View.VISIBLE : View.GONE);
        if (prefs != null) {
            prefs.edit().putString(KEY_OP, spec.id).apply();
        }
        populateTargets(spec);
        refreshMismatchFlags();
    }

    private void populateTargets(OpSpec spec) {
        boolean hasTargets = spec != null && !spec.targets.isEmpty();
        labelTarget.setVisibility(hasTargets ? View.VISIBLE : View.GONE);
        chipTarget.setVisibility(hasTargets ? View.VISIBLE : View.GONE);

        updating = true;
        chipTarget.removeAllViews();
        currentTarget = "";
        if (hasTargets) {
            for (String target : spec.targets) {
                Chip chip = buildChip(target.toUpperCase(Locale.ROOT));
                chip.setTag(target);
                chipTarget.addView(chip);
            }
            String lastTarget = prefs == null ? null : prefs.getString(KEY_TARGET, null);
            String pick = spec.targets.get(0);
            if (lastTarget != null && spec.targets.contains(lastTarget)) {
                pick = lastTarget;
            }
            checkChipWithTag(chipTarget, pick);
            currentTarget = pick;
        }
        updating = false;
    }

    private Chip buildChip(String text) {
        Chip chip = new Chip(this);
        chip.setText(text);
        chip.setCheckable(true);
        chip.setId(View.generateViewId());
        return chip;
    }

    private void checkChipWithTag(ChipGroup group, String tag) {
        for (int i = 0; i < group.getChildCount(); i++) {
            View child = group.getChildAt(i);
            if (tag != null && tag.equals(child.getTag()) && child instanceof Chip) {
                ((Chip) child).setChecked(true);
                return;
            }
        }
    }

    private OpSpec findSpec(String id) {
        for (OpSpec spec : specs) {
            if (spec.id.equals(id)) {
                return spec;
            }
        }
        return null;
    }

    /** 离线可用的转换：图片、PDF、结构化数据；Office/音视频依赖外部二进制，Android 端不提供。 */
    private boolean isSupported(JSONObject item) {
        String id = item.optString("id");
        if ("table_to_json".equals(id) || "word_to_text".equals(id)) {
            return true;
        }
        String src = item.optString("src");
        return "图片".equals(src) || "PDF".equals(src) || "数据".equals(src);
    }

    /** Excel/CSV 转 JSON 只有一条，归到「数据」类别里，避免出现第四个孤立分组。 */
    private static String categoryOf(String src) {
        if ("Excel".equals(src)) {
            return "数据";
        }
        if ("Word".equals(src)) {
            return "文档";
        }
        return src;
    }

    /** Android 专属的转换项：不走 C++ 核心，直接用系统媒体 API。 */
    private void addAndroidOnlySpecs() {
        for (OpSpec existing : specs) {
            if (existing.id.startsWith("android_")) {
                return;
            }
        }
        List<String> videoExts = Arrays.asList(".mp4", ".m4v", ".mkv", ".mov", ".3gp", ".webm", ".ts");
        List<String> audioExts = Arrays.asList(".mp3", ".m4a", ".aac", ".wav", ".ogg", ".opus",
                ".flac", ".amr", ".mp4", ".m4v", ".mkv", ".mov", ".webm");
        specs.add(new OpSpec("android_extract_audio", "提取音轨（M4A）", "音视频", "音视频",
                Collections.singletonList("m4a"), videoExts, true, false));
        specs.add(new OpSpec("android_video_to_gif", "视频转 GIF", "音视频", "图片",
                Collections.emptyList(), videoExts, false, false));
        specs.add(new OpSpec("android_video_transcode", "视频转 MP4（H.264）", "音视频", "音视频",
                Collections.singletonList("mp4"), videoExts, true, false));
        specs.add(new OpSpec("android_audio_transcode", "音频转 M4A（AAC）", "音视频", "音视频",
                Collections.singletonList("m4a"), audioExts, true, false));
        specs.add(new OpSpec("android_audio_mp3", "音频转 MP3", "音视频", "音视频",
                Collections.singletonList("mp3"), audioExts, true, false));
    }

    private String stemOf(File file) {
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    /** 音视频子集：逐文件调用 MediaConverter，其余逻辑（进度、入库、分享）与核心任务一致。 */
    private void startMediaConversion(OpSpec spec) {
        final boolean wantPublish = chkPublish.isChecked();
        beginJob(entries.size());
        for (Entry entry : entries) {
            entry.state = Entry.STATE_WAITING;
            entry.status = getString(R.string.item_waiting);
        }
        adapter.notifyDataSetChanged();
        appendLog("开始：" + OpLabels.name(spec.id, shortLabel(spec.label))
                + "（" + entries.size() + " 个文件）");

        executor.execute(() -> {
            List<Uri> published = new ArrayList<>();
            for (Entry entry : entries) {
                final String path = entry.file.getAbsolutePath();
                try {
                    File produced;
                    switch (spec.id) {
                        case "android_extract_audio":
                            produced = MediaConverter.extractAudio(entry.file, outputDir,
                                    stemOf(entry.file));
                            break;
                        case "android_video_to_gif":
                            produced = MediaConverter.videoToGif(entry.file, outputDir,
                                    stemOf(entry.file), baseDir);
                            break;
                        case "android_video_transcode":
                            produced = Media3Transcoder.toMp4(this, entry.file, outputDir,
                                    stemOf(entry.file), this::onTranscodeProgress);
                            break;
                        case "android_audio_mp3":
                            produced = AudioToMp3.convert(this, entry.file, outputDir,
                                    stemOf(entry.file));
                            break;
                        default:
                            produced = Media3Transcoder.toM4a(this, entry.file, outputDir,
                                    stemOf(entry.file), this::onTranscodeProgress);
                            break;
                    }
                    final String name = produced.getName();
                    synchronized (pendingOutputs) {
                        pendingOutputs.add(produced);
                    }
                    if (wantPublish) {
                        try {
                            Uri uri = OutputPublisher.publish(this, produced,
                                    OutputPublisher.mimeOf(name));
                            if (uri != null) {
                                published.add(uri);
                            }
                        } catch (Exception exc) {
                            final String message = "保存到媒体库失败：" + exc.getMessage();
                            ui.post(() -> appendLog(message));
                        }
                    }
                    ui.post(() -> {
                        setEntryState(path, Entry.STATE_DONE,
                                getString(R.string.item_done_to, name));
                        bumpProgress();
                    });
                } catch (Exception exc) {
                    final String reason = exc.getMessage() == null
                            ? String.valueOf(exc) : exc.getMessage();
                    ui.post(() -> {
                        setEntryState(path, Entry.STATE_FAILED,
                                getString(R.string.item_failed, reason));
                        appendLog("失败：" + entry.file.getName() + "：" + reason);
                        showLogPanel(true);
                        bumpProgress();
                    });
                }
            }
            final List<Uri> publishedList = published;
            ui.post(() -> finishMediaJob(publishedList));
        });
    }

    /** Media3 转码进度（0-100），同步到通知栏。 */
    private void onTranscodeProgress(int percent) {
        int total = Math.max(1, totalTasks);
        ConversionService.updateProgress(this, Math.min(doneTasks, total), total,
                getString(R.string.notify_transcoding, percent));
    }

    private void finishMediaJob(List<Uri> published) {
        if (!published.isEmpty()) {
            publishedUris.clear();
            publishedUris.addAll(published);
            btnShare.setEnabled(true);
            appendLog("已保存 " + published.size()
                    + " 个文件（图片在 Pictures/" + OutputPublisher.FOLDER
                    + "，其它在 Download/" + OutputPublisher.FOLDER + "）");
        }
        endJob();
    }
    /** 注册表里的 label 带了括号说明，胶囊上只留主标题。 */
    private static String shortLabel(String label) {
        int index = label.indexOf('（');
        return index > 0 ? label.substring(0, index).trim() : label;
    }

    // ------------------------------------------------------------------
    // 选择文件
    // ------------------------------------------------------------------

    private void pickFiles() {
        OpSpec spec = currentSpec;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        String type = "*/*";
        String[] mimes = null;
        if (spec != null) {
            if ("图片".equals(spec.src)) {
                type = "image/*";
            } else if ("PDF".equals(spec.src)) {
                type = "application/pdf";
            } else if ("音视频".equals(spec.src)) {
                type = "video/*";
            } else {
                mimes = mimeTypesFor(spec.extensions);
                if (mimes.length > 0) {
                    intent.putExtra(Intent.EXTRA_MIME_TYPES, mimes);
                }
            }
        }
        intent.setType(type);
        startActivityForResult(intent, REQUEST_FILES);
    }

    /** 把转换项支持的扩展名映射成 MIME，选择器就不会列出完全无关的文件。 */
    private String[] mimeTypesFor(List<String> extensions) {
        java.util.LinkedHashSet<String> types = new java.util.LinkedHashSet<>();
        for (String ext : extensions) {
            switch (ext.toLowerCase(Locale.ROOT)) {
                case ".json":
                    types.add("application/json");
                    types.add("text/plain");
                    break;
                case ".xml":
                    types.add("application/xml");
                    types.add("text/xml");
                    break;
                case ".yaml":
                case ".yml":
                    types.add("application/yaml");
                    types.add("text/yaml");
                    types.add("text/plain");
                    break;
                case ".xlsx":
                    types.add("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                    break;
                case ".csv":
                    types.add("text/csv");
                    types.add("text/comma-separated-values");
                    types.add("text/plain");
                    break;
                default:
                    types.add("application/octet-stream");
                    break;
            }
        }
        return types.toArray(new String[0]);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_FILES || resultCode != RESULT_OK || data == null) {
            return;
        }
        List<Uri> uris = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null) {
            for (int i = 0; i < clip.getItemCount(); i++) {
                uris.add(clip.getItemAt(i).getUri());
            }
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        addUris(uris);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    @SuppressWarnings("deprecation")
    private void handleIntent(Intent intent) {
        if (intent == null) {
            return;
        }
        String action = intent.getAction();
        if (Intent.ACTION_SEND.equals(action)) {
            Object extra = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            Uri uri = extra instanceof Uri ? (Uri) extra : intent.getData();
            if (uri != null) {
                addUris(Collections.singletonList(uri));
            }
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(action)) {
            ArrayList<Uri> uris = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (uris != null) {
                addUris(uris);
            }
        } else if (Intent.ACTION_VIEW.equals(action) && intent.getData() != null) {
            addUris(Collections.singletonList(intent.getData()));
        }
    }

    private void addUris(List<Uri> uris) {
        if (uris.isEmpty()) {
            return;
        }
        final OpSpec specAtAdd = currentSpec;
        txtStatus.setText(R.string.status_reading);
        executor.execute(() -> {
            for (Uri uri : uris) {
                try {
                    File file = copyToCache(uri);
                    final Entry entry = new Entry(file, getString(R.string.item_ready));
                    if (specAtAdd != null && !specAtAdd.extensions.isEmpty()
                            && !matchesExtensions(file, specAtAdd.extensions)) {
                        entry.state = Entry.STATE_MISMATCH;
                        entry.status = getString(R.string.item_mismatch);
                    }
                    ui.post(() -> {
                        entries.add(entry);
                        adapter.notifyDataSetChanged();
                        updateFileSummary();
                    });
                } catch (Exception exc) {
                    final String message = "读取文件失败：" + exc;
                    ui.post(() -> appendLog(message));
                }
            }
            ui.post(() -> {
                txtStatus.setText(R.string.status_ready);
                updateFileSummary();
            });
        });
    }

    /** 文件名扩展名是否在转换项允许的范围内；用来提前提示，而不是等到转换才报错。 */
    private boolean matchesExtensions(File file, List<String> extensions) {
        String name = file.getName().toLowerCase(Locale.ROOT);
        for (String ext : extensions) {
            if (name.endsWith(ext.toLowerCase(Locale.ROOT))) {
                return true;
            }
        }
        return false;
    }

    /** 切换转换方式后，重新标注队列里类型不对的文件（已完成 / 已失败的保持原状态）。 */
    private void refreshMismatchFlags() {
        OpSpec spec = currentSpec;
        boolean changed = false;
        for (Entry entry : entries) {
            if (entry.state == Entry.STATE_DONE || entry.state == Entry.STATE_FAILED) {
                continue;
            }
            boolean mismatch = spec != null && !spec.extensions.isEmpty()
                    && !matchesExtensions(entry.file, spec.extensions);
            int state = mismatch ? Entry.STATE_MISMATCH : Entry.STATE_READY;
            if (entry.state != state) {
                entry.state = state;
                entry.status = mismatch
                        ? getString(R.string.item_mismatch)
                        : getString(R.string.item_ready);
                changed = true;
            }
        }
        if (changed) {
            adapter.notifyDataSetChanged();
        }
    }

    private void clearEntries() {
        if (running) {
            toast(getString(R.string.toast_busy_clear));
            return;
        }
        entries.clear();
        adapter.notifyDataSetChanged();
        updateFileSummary();
    }

    private void updateFileSummary() {
        txtQueueCount.setText(entries.isEmpty()
                ? getString(R.string.queue_count_empty)
                : getString(R.string.queue_count_n, entries.size()));
        updateQueueVisibility();
    }

    private File copyToCache(Uri uri) throws Exception {
        String name = queryDisplayName(uri);
        String safe = name.replaceAll("[\\\\/:*?\"<>|]", "_");
        if (safe.isEmpty()) {
            safe = "input.bin";
        }
        int dot = safe.lastIndexOf('.');
        String stem = dot > 0 ? safe.substring(0, dot) : safe;
        String ext = dot > 0 ? safe.substring(dot) : "";
        File dir = new File(getCacheDir(), "inputs");
        if (!dir.exists() && !dir.mkdirs()) {
            throw new Exception("无法创建缓存目录");
        }
        File out = new File(dir, safe);
        int index = 1;
        while (out.exists()) {
            out = new File(dir, stem + "_" + index + ext);
            index++;
        }
        try (InputStream in = getContentResolver().openInputStream(uri);
             FileOutputStream stream = new FileOutputStream(out)) {
            if (in == null) {
                throw new Exception("无法打开输入流");
            }
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                stream.write(buffer, 0, read);
            }
        }
        return out;
    }

    @SuppressWarnings("deprecation")
    private String queryDisplayName(Uri uri) {
        String name = null;
        try (android.database.Cursor cursor = getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) {
                    name = cursor.getString(index);
                }
            }
        } catch (Exception ignored) {
        }
        if (name == null) {
            name = uri.getLastPathSegment();
        }
        if (name == null) {
            return "input.bin";
        }
        int slash = name.lastIndexOf('/');
        return slash >= 0 ? name.substring(slash + 1) : name;
    }

    // ------------------------------------------------------------------
    // 转换
    // ------------------------------------------------------------------

    private void startConversion() {
        if (running) {
            return;
        }
        if (entries.isEmpty()) {
            toast(getString(R.string.toast_pick_first));
            return;
        }
        OpSpec spec = currentSpec;
        if (spec == null) {
            toast(getString(R.string.toast_no_op));
            return;
        }
        String target = spec.targets.isEmpty() ? "" : currentTarget;
        if (!spec.targets.isEmpty() && target.isEmpty()) {
            toast(getString(R.string.toast_pick_target));
            return;
        }
        final boolean wantPublish = chkPublish.isChecked();
        if (wantPublish && Build.VERSION.SDK_INT < 29
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    REQUEST_WRITE_STORAGE);
            toast(getString(R.string.toast_need_storage));
            return;
        }

        if (spec.id.startsWith("android_")) {
            startMediaConversion(spec);
            return;
        }

        final String jobText;
        try {
            jobText = buildJob(spec, target).toString();
        } catch (Exception exc) {
            appendLog("构造任务失败：" + exc);
            return;
        }
        if (prefs != null && !target.isEmpty()) {
            prefs.edit().putString(KEY_TARGET, target).apply();
        }

        beginJob(entries.size());
        for (Entry entry : entries) {
            entry.state = Entry.STATE_WAITING;
            entry.status = getString(R.string.item_waiting);
        }
        adapter.notifyDataSetChanged();
        appendLog("开始：" + spec.label + (target.isEmpty() ? "" : " → " + target)
                + "（" + entries.size() + " 个文件）");

        executor.execute(() -> {
            String result;
            try {
                result = NativeCore.runJobStreaming(jobText, baseDir, this::onEvent);
            } catch (Throwable exc) {
                final String message = "执行失败：" + exc;
                ui.post(() -> {
                    appendLog(message);
                    endJob();
                });
                return;
            }
            List<File> outputs;
            synchronized (pendingOutputs) {
                outputs = new ArrayList<>(pendingOutputs);
            }
            final List<Uri> published = new ArrayList<>();
            if (wantPublish) {
                for (File file : outputs) {
                    try {
                        Uri uri = OutputPublisher.publish(MainActivity.this, file,
                                OutputPublisher.mimeOf(file.getName()));
                        if (uri != null) {
                            published.add(uri);
                        }
                    } catch (Exception exc) {
                        final String message = "保存到媒体库失败：" + exc.getMessage();
                        ui.post(() -> appendLog(message));
                    }
                }
            }
            final String resultText = result;
            final int outputCount = outputs.size();
            ui.post(() -> finishJob(resultText, published, outputCount, wantPublish));
        });
    }

    private JSONObject buildJob(OpSpec spec, String target) throws Exception {
        JSONObject job = new JSONObject();
        job.put("op", spec.id);
        JSONArray inputs = new JSONArray();
        for (Entry entry : entries) {
            inputs.put(entry.file.getAbsolutePath());
        }
        job.put("inputs", inputs);
        job.put("out_dir", outputDir.getAbsolutePath());
        if (!target.isEmpty()) {
            job.put("target", target);
        }
        JSONObject options = new JSONObject();
        if ("image_convert".equals(spec.id)) {
            options.put("quality", (int) sliderQuality.getValue());
        }
        if ("pdf_to_image".equals(spec.id)) {
            options.put("dpi", Integer.parseInt(currentDpi));
        }
        if (options.length() > 0) {
            job.put("options", options);
        }
        return job;
    }

    /** 由 C++ 工作线程调用：先把事件切到主线程，再做 UI 更新。 */
    private void onEvent(String eventJson) {
        JSONObject event;
        try {
            event = new JSONObject(eventJson);
        } catch (Exception exc) {
            return;
        }
        String type = event.optString("type");
        String input = event.optString("input");
        if ("file_ok".equals(type)) {
            List<String> names = new ArrayList<>();
            JSONArray outputs = event.optJSONArray("outputs");
            if (outputs != null) {
                for (int i = 0; i < outputs.length(); i++) {
                    File file = new File(outputs.optString(i));
                    names.add(file.getName());
                    synchronized (pendingOutputs) {
                        pendingOutputs.add(file);
                    }
                }
            }
            final String status = names.isEmpty()
                    ? getString(R.string.item_done)
                    : getString(R.string.item_done_to, String.join("、", names));
            ui.post(() -> {
                setEntryState(input, Entry.STATE_DONE, status);
                bumpProgress();
            });
        } else if ("file_error".equals(type)) {
            final String status = getString(R.string.item_failed, event.optString("error"));
            ui.post(() -> {
                setEntryState(input, Entry.STATE_FAILED, status);
                appendLog("失败：" + new File(input).getName() + "：" + event.optString("error"));
                showLogPanel(true);
                bumpProgress();
            });
        } else if ("message".equals(type)) {
            final String message = event.optString("message");
            ui.post(() -> appendLog(message));
        } else if ("done".equals(type)) {
            final int ok = event.optInt("ok");
            final int failed = event.optInt("failed");
            final boolean cancelled = event.optBoolean("cancelled");
            ui.post(() -> appendLog("任务结束：成功 " + ok + "，失败 " + failed
                    + (cancelled ? "（已取消）" : "")));
        }
    }

    private void finishJob(String resultText, List<Uri> published, int outputCount,
                           boolean wantPublish) {
        try {
            JSONObject result = new JSONObject(resultText);
            int code = result.optInt("code", 2);
            if (code == 2) {
                appendLog("任务失败：" + result.optString("error"));
                showLogPanel(true);
            }
        } catch (Exception exc) {
            appendLog("解析结果失败：" + exc);
        }
        if (!published.isEmpty()) {
            publishedUris.clear();
            publishedUris.addAll(published);
            btnShare.setEnabled(true);
            appendLog("已保存 " + published.size()
                    + " 个文件（图片在 Pictures/" + OutputPublisher.FOLDER
                    + "，其它在 Download/" + OutputPublisher.FOLDER + "）");
        } else if (wantPublish && outputCount > 0) {
            appendLog("结果未写入系统媒体库，文件仍在：" + outputDir.getAbsolutePath());
        }
        endJob();
    }

    private void cancelJob() {
        if (!running) {
            return;
        }
        NativeCore.cancel();
        appendLog(getString(R.string.log_cancel_requested));
        txtStatus.setText(R.string.status_cancelling);
    }

    private void shareOutputs() {
        if (publishedUris.isEmpty()) {
            toast(getString(R.string.toast_no_results));
            return;
        }
        try {
            String mime = publishedUris.size() == 1
                    ? getContentResolver().getType(publishedUris.get(0))
                    : "*/*";
            Intent intent = OutputPublisher.shareIntent(new ArrayList<>(publishedUris),
                    mime == null ? "*/*" : mime);
            startActivity(Intent.createChooser(intent, getString(R.string.share_chooser)));
        } catch (Exception exc) {
            appendLog("分享失败：" + exc);
        }
    }

    // ------------------------------------------------------------------
    // 日志面板
    // ------------------------------------------------------------------

    private void toggleLogPanel() {
        showLogPanel(logPanel.getVisibility() != View.VISIBLE);
    }

    private void showLogPanel(boolean show) {
        logPanel.setVisibility(show ? View.VISIBLE : View.GONE);
        btnToggleLog.setText(show ? R.string.log_collapse : R.string.log_toggle);
        ViewGroup.LayoutParams params = logPanel.getLayoutParams();
        if (params instanceof LinearLayout.LayoutParams) {
            LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) params;
            lp.height = 0;
            lp.weight = show ? 1f : 0f;
            logPanel.setLayoutParams(lp);
        }
        updateQueueVisibility();
        if (show) {
            scrollLogToBottom();
        }
    }

    /** 队列列表 / 空提示 / 日志三者同一时间只显示一个，避免在小屏上互相挤压。 */
    private void updateQueueVisibility() {
        boolean logVisible = logPanel.getVisibility() == View.VISIBLE;
        boolean empty = entries.isEmpty();
        listFiles.setVisibility(!logVisible && !empty ? View.VISIBLE : View.GONE);
        txtEmptyQueue.setVisibility(!logVisible && empty ? View.VISIBLE : View.GONE);
    }

    private void scrollLogToBottom() {
        logPanel.post(() -> {
            View child = logPanel.getChildAt(0);
            if (child != null) {
                logPanel.smoothScrollTo(0, child.getBottom());
            }
        });
    }

    // ------------------------------------------------------------------
    // 进度
    // ------------------------------------------------------------------

    private void beginJob(int total) {
        running = true;
        totalTasks = total;
        doneTasks = 0;
        synchronized (pendingOutputs) {
            pendingOutputs.clear();
        }
        progress.setIndeterminate(total <= 0);
        progress.setMax(Math.max(1, total));
        progress.setProgress(0);
        progress.setVisibility(View.VISIBLE);
        btnRun.setEnabled(false);
        btnPick.setEnabled(false);
        btnClear.setEnabled(false);
        btnSelfTest.setEnabled(false);
        btnCancel.setEnabled(true);
        chipCategory.setEnabled(false);
        chipOp.setEnabled(false);
        chipTarget.setEnabled(false);
        txtStatus.setText(total <= 0
                ? getString(R.string.status_self_test)
                : getString(R.string.status_converting, 0, total));
        // 前台服务保证退到后台时进程不被回收，同时在通知栏显示进度。
        ConversionService.start(this);
    }

    private void endJob() {
        running = false;
        progress.setVisibility(View.INVISIBLE);
        progress.setIndeterminate(false);
        btnRun.setEnabled(true);
        btnPick.setEnabled(true);
        btnClear.setEnabled(true);
        btnSelfTest.setEnabled(true);
        btnCancel.setEnabled(false);
        chipCategory.setEnabled(true);
        chipOp.setEnabled(true);
        chipTarget.setEnabled(true);
        txtStatus.setText(R.string.status_ready);
        ConversionService.stop(this);
    }

    private void bumpProgress() {
        doneTasks++;
        progress.setProgress(Math.min(doneTasks, Math.max(1, totalTasks)));
        if (totalTasks > 0) {
            String text = getString(R.string.status_converting,
                    Math.min(doneTasks, totalTasks), totalTasks);
            txtStatus.setText(text);
            ConversionService.updateProgress(this, Math.min(doneTasks, totalTasks),
                    totalTasks, txtStatus.getText().toString());
        }
    }

    private void setEntryState(String inputPath, int state, String status) {
        for (Entry entry : entries) {
            if (entry.file.getAbsolutePath().equals(inputPath)) {
                entry.state = state;
                entry.status = status;
                adapter.notifyDataSetChanged();
                return;
            }
        }
    }

    // ------------------------------------------------------------------
    // 设备自检
    // ------------------------------------------------------------------

    private void startSelfTest() {
        if (running) {
            toast(getString(R.string.toast_busy));
            return;
        }
        beginJob(0);
        showLogPanel(true);
        appendLog("===== 设备自检开始 =====");
        executor.execute(() -> {
            try {
                runSelfTest();
            } catch (Throwable exc) {
                log("❌ 自检异常：" + exc);
            } finally {
                ui.post(() -> {
                    appendLog("===== 设备自检结束 =====");
                    endJob();
                });
            }
        });
    }

    private void runSelfTest() {
        try {
            log("核心版本：" + NativeCore.version());
        } catch (Throwable exc) {
            log("❌ 原生库加载失败：" + exc);
            return;
        }
        File root = new File(getCacheDir(), "selftest");
        deleteRecursively(root);
        File in = new File(root, "in");
        File out = new File(root, "out");
        if (!ensureDirectory(in) || !ensureDirectory(out)) {
            log("❌ 无法创建自检目录");
            return;
        }
        int step = 0;
        try {
            File png = makeSample(new File(in, "sample.png"), Bitmap.CompressFormat.PNG, 100);
            File jpg = makeSample(new File(in, "sample.jpg"), Bitmap.CompressFormat.JPEG, 90);
            File json = writeText(new File(in, "sample.json"),
                    "{\"name\":\"格式转换工具\",\"items\":[{\"id\":1,\"size\":12.5},"
                            + "{\"id\":2,\"size\":3}],\"tags\":[\"自检\",\"android\"]}");
            File bridge = new File(in, "bridge.heic");
            copyFile(png, bridge);

            runStep(++step, out, "图片转 WebP", "image_convert", "webp", false, png);
            runStep(++step, out, "图片转 ICO", "image_convert", "ico", false, png);
            runStep(++step, out, "图片转 GIF", "image_convert", "gif", false, png);
            runStep(++step, out, "解码桥（系统解码器通道）", "image_convert", "png", false, bridge);
            File pdf = runStep(++step, out, "图片转 PDF", "image_to_pdf", null, false, png);
            if (pdf != null) {
                File rotated = runStep(++step, out, "PDF 旋转 90°", "pdf_rotate", "90", false, pdf);
                runStep(++step, out, "PDF 转图片", "pdf_to_image", "png", false, pdf);
                runStep(++step, out, "PDF 转 Word", "pdf_to_word", null, false, pdf);
                runStep(++step, out, "PDF 转 Excel", "pdf_to_excel", null, true, pdf);
                runStep(++step, out, "PDF 转 PPT", "pdf_to_ppt", null, false, pdf);
                runStep(++step, out, "PDF 转文本", "pdf_to_text", null, true, pdf);
                runStep(++step, out, "PDF 拆分", "pdf_split", null, false, pdf);
                runStep(++step, out, "PDF 压缩", "pdf_compress", null, false, pdf);
                if (rotated != null) {
                    runStep(++step, out, "PDF 合并", "pdf_merge", null, false, pdf, rotated);
                }
            } else {
                log("⚠️ 图片转 PDF 未通过，跳过 PDF 相关步骤");
            }
            File xlsx = runStep(++step, out, "JSON 转 Excel", "data_convert", "xlsx", false, json);
            if (xlsx != null) {
                runStep(++step, out, "Excel 转 JSON", "table_to_json", null, false, xlsx);
            }
            runStep(++step, out, "JSON 转 YAML", "data_convert", "yaml", false, json);
            runStep(++step, out, "图片合成动图", "images_to_gif", null, false, png, jpg);
        } catch (Throwable exc) {
            log("❌ 自检中断：" + exc);
        }
    }

    private File runStep(int index, File root, String label, String op, String target,
                         boolean allowEmpty, File... inputs) {
        File outDir = new File(root, "step" + index);
        if (!ensureDirectory(outDir)) {
            log("❌ " + label + "：无法创建输出目录");
            return null;
        }
        JSONObject job = new JSONObject();
        try {
            job.put("op", op);
            JSONArray arr = new JSONArray();
            for (File file : inputs) {
                arr.put(file.getAbsolutePath());
            }
            job.put("inputs", arr);
            job.put("out_dir", outDir.getAbsolutePath());
            if (target != null && !target.isEmpty()) {
                job.put("target", target);
            }
            JSONObject options = new JSONObject();
            if ("pdf_to_image".equals(op)) {
                options.put("dpi", 96);
            }
            if (options.length() > 0) {
                job.put("options", options);
            }
        } catch (Exception exc) {
            log("❌ " + label + "：构造任务失败 " + exc);
            return null;
        }
        final AtomicReference<String> failure = new AtomicReference<>();
        String result = null;
        try {
            result = NativeCore.runJobStreaming(job.toString(), baseDir, eventJson -> {
                try {
                    JSONObject event = new JSONObject(eventJson);
                    if ("file_error".equals(event.optString("type"))) {
                        failure.set(event.optString("error"));
                    }
                } catch (Exception ignored) {
                }
            });
        } catch (Throwable exc) {
            failure.set(String.valueOf(exc));
        }
        if (failure.get() != null) {
            if (allowEmpty && (failure.get().contains("未提取到文本")
                    || failure.get().contains("未提取到任何表格或文本"))) {
                log("ℹ️ " + label + "：图片页没有文字层，符合预期");
                return null;
            }
            log("❌ " + label + "：" + failure.get());
            return null;
        }
        File[] files = outDir.listFiles();
        if (files == null || files.length == 0) {
            if (allowEmpty) {
                log("ℹ️ " + label + "：没有文字层，输出为空（符合预期）");
                return null;
            }
            log("❌ " + label + "：没有生成输出");
            return null;
        }
        File first = files[0];
        if (first.length() <= 0) {
            log("❌ " + label + "：输出文件为空");
            return null;
        }
        String code = "?";
        try {
            code = String.valueOf(new JSONObject(result).optInt("code", 2));
        } catch (Exception ignored) {
        }
        log("✅ " + label + " → " + first.getName() + "（" + humanSize(first.length())
                + "，code=" + code + "）");
        return first;
    }

    private File makeSample(File target, Bitmap.CompressFormat format, int quality) throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(480, 320, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(bitmap);
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
            paint.setShader(new LinearGradient(0, 0, 480, 320, 0xFF3B62F6, 0xFF00C2A8,
                    Shader.TileMode.CLAMP));
            canvas.drawRect(0, 0, 480, 320, paint);
            paint.setShader(null);
            paint.setColor(0xFFFFFFFF);
            paint.setTextSize(52f);
            canvas.drawText("Format", 36, 150, paint);
            paint.setTextSize(28f);
            canvas.drawText("Android self-test 480x320", 36, 208, paint);
            try (FileOutputStream stream = new FileOutputStream(target)) {
                if (!bitmap.compress(format, quality, stream)) {
                    throw new Exception("样张编码失败");
                }
            }
        } finally {
            bitmap.recycle();
        }
        return target;
    }

    private File writeText(File target, String text) throws Exception {
        try (FileOutputStream stream = new FileOutputStream(target)) {
            stream.write(text.getBytes("UTF-8"));
        }
        return target;
    }

    private void copyFile(File source, File target) throws Exception {
        try (InputStream in = new FileInputStream(source);
             OutputStream out = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = in.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
        }
    }

    private static String humanSize(long bytes) {
        if (bytes < 1024) {
            return bytes + " B";
        }
        if (bytes < 1024 * 1024) {
            return String.format(Locale.ROOT, "%.1f KB", bytes / 1024.0);
        }
        return String.format(Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024.0));
    }

    private boolean ensureDirectory(File dir) {
        return dir.exists() || dir.mkdirs();
    }

    private void deleteRecursively(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    // ------------------------------------------------------------------
    // 杂项
    // ------------------------------------------------------------------

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == REQUEST_WRITE_STORAGE && results.length > 0
                && results[0] == PackageManager.PERMISSION_GRANTED) {
            startConversion();
        }
    }

    private void appendLog(String text) {
        txtLog.append(text + "\n");
        if (logPanel.getVisibility() == View.VISIBLE) {
            scrollLogToBottom();
        }
    }

    private void log(String text) {
        ui.post(() -> appendLog(text));
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_SHORT).show();
    }

    @Override
    protected void onDestroy() {
        executor.shutdownNow();
        super.onDestroy();
    }

    private static final class Entry {
        static final int STATE_READY = 0;
        static final int STATE_WAITING = 1;
        static final int STATE_DONE = 2;
        static final int STATE_FAILED = 3;
        static final int STATE_MISMATCH = 4;

        final File file;
        String status;
        int state = STATE_READY;

        Entry(File file, String status) {
            this.file = file;
            this.status = status;
        }
    }

    private static final class OpSpec {
        final String id;
        final String label;
        final String src;
        final String dst;
        final List<String> targets;
        final List<String> extensions;
        final boolean imageTargets;
        final boolean showQuality;

        OpSpec(String id, String label, String src, String dst, List<String> targets,
               List<String> extensions, boolean imageTargets, boolean showQuality) {
            this.id = id;
            this.label = label;
            this.src = src;
            this.dst = dst;
            this.targets = targets;
            this.extensions = extensions;
            this.imageTargets = imageTargets;
            this.showQuality = showQuality;
        }
    }

    private final class EntryAdapter extends BaseAdapter {

        @Override
        public int getCount() {
            return entries.size();
        }

        @Override
        public Entry getItem(int position) {
            return entries.get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            View view = convertView;
            if (view == null) {
                view = getLayoutInflater().inflate(R.layout.item_queue, parent, false);
            }
            Entry entry = getItem(position);
            TextView icon = view.findViewById(R.id.itemIcon);
            TextView name = view.findViewById(R.id.itemName);
            TextView status = view.findViewById(R.id.itemStatus);

            icon.setText(extensionLabel(entry.file.getName()));
            name.setText(entry.file.getName());
            status.setText(entry.status);

            int textColor;
            int containerColor;
            switch (entry.state) {
                case Entry.STATE_FAILED:
                    textColor = R.color.app_error;
                    containerColor = R.color.app_error_container;
                    break;
                case Entry.STATE_DONE:
                    textColor = R.color.app_success;
                    containerColor = R.color.app_success_container;
                    break;
                case Entry.STATE_WAITING:
                case Entry.STATE_MISMATCH:
                    textColor = R.color.app_warn;
                    containerColor = R.color.app_warn_container;
                    break;
                default:
                    textColor = R.color.app_muted;
                    containerColor = R.color.app_surface_variant;
                    break;
            }
            int text = ContextCompat.getColor(MainActivity.this, textColor);
            int container = ContextCompat.getColor(MainActivity.this, containerColor);
            status.setTextColor(text);
            icon.setTextColor(text);
            icon.setBackgroundTintList(ColorStateList.valueOf(container));
            return view;
        }

        private String extensionLabel(String fileName) {
            int dot = fileName.lastIndexOf('.');
            if (dot < 0 || dot >= fileName.length() - 1) {
                return "FILE";
            }
            String ext = fileName.substring(dot + 1).toUpperCase(Locale.ROOT);
            return ext.length() > 4 ? ext.substring(0, 4) : ext;
        }
    }
}
