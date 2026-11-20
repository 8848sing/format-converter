package com.formatconverter.app;

import android.content.Intent;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Bundle;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

/**
 * 关于页：版本、核心能力、开源组件、设备信息，以及崩溃日志的导出/清除。
 *
 * <p>没有网络权限，所以这里只有本地信息；「导出」走系统分享，把文本发给用户选的应用。
 */
public class AboutActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_about);

        findViewById(R.id.btnBack).setOnClickListener(v -> finish());

        ((TextView) findViewById(R.id.txtAboutVersion)).setText(buildVersionText());
        ((TextView) findViewById(R.id.txtAboutDevice)).setText(buildDeviceText());
        ((TextView) findViewById(R.id.txtAboutCrash)).setText(buildCrashText());

        findViewById(R.id.btnExport).setOnClickListener(v -> exportDiagnostics());
        findViewById(R.id.btnClearCrash).setOnClickListener(v -> {
            FmtApplication.clearCrash(this);
            ((TextView) findViewById(R.id.txtAboutCrash)).setText(getString(R.string.about_crash_none));
        });
    }

    private String buildVersionText() {
        String versionName = "?";
        int targetSdk = 0;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            versionName = info.versionName;
            targetSdk = info.applicationInfo.targetSdkVersion;
        } catch (Exception ignored) {
        }
        String core;
        try {
            core = NativeCore.version();
        } catch (Throwable exc) {
            core = "不可用：" + exc;
        }
        return getString(R.string.about_app_version, versionName)
                + "\n" + getString(R.string.about_core_version, core)
                + "\n" + getString(R.string.about_target_sdk, targetSdk);
    }

    private String buildDeviceText() {
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?";
        return getString(R.string.about_device_line, Build.MANUFACTURER, Build.MODEL,
                Build.VERSION.RELEASE, abi);
    }

    private String buildCrashText() {
        if (!FmtApplication.hasCrash(this)) {
            return getString(R.string.about_crash_none);
        }
        String crash = FmtApplication.readCrash(this);
        return crash.length() > 800 ? crash.substring(0, 800) + "…" : crash;
    }

    private void exportDiagnostics() {
        String text = "格式转换工具 诊断信息\n\n"
                + buildVersionText() + "\n\n"
                + buildDeviceText() + "\n\n"
                + (FmtApplication.hasCrash(this)
                ? "崩溃日志：\n" + FmtApplication.readCrash(this)
                : "无崩溃记录");
        Intent intent = new Intent(Intent.ACTION_SEND);
        intent.setType("text/plain");
        intent.putExtra(Intent.EXTRA_SUBJECT, "格式转换工具 诊断信息");
        intent.putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(intent, getString(R.string.about_export)));
    }
}