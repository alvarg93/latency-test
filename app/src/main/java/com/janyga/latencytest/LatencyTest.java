package com.janyga.latencytest;

import android.Manifest;
import android.annotation.TargetApi;
import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.support.annotation.NonNull;
import android.support.v4.app.ActivityCompat;
import android.support.v7.app.AlertDialog;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.Toast;

import com.github.mikephil.charting.charts.BarChart;
import com.github.mikephil.charting.charts.LineChart;
import com.github.mikephil.charting.components.Description;
import com.github.mikephil.charting.components.XAxis;
import com.github.mikephil.charting.components.YAxis;
import com.github.mikephil.charting.data.BarData;
import com.github.mikephil.charting.data.BarDataSet;
import com.github.mikephil.charting.data.BarEntry;
import com.github.mikephil.charting.data.Entry;
import com.github.mikephil.charting.data.LineData;
import com.github.mikephil.charting.data.LineDataSet;
import com.google.firebase.database.DataSnapshot;
import com.google.firebase.database.DatabaseError;
import com.google.firebase.database.DatabaseReference;
import com.google.firebase.database.FirebaseDatabase;
import com.google.firebase.database.ValueEventListener;

import java.math.BigDecimal;
import java.util.ArrayList;
import java.util.List;

import butterknife.BindView;
import butterknife.ButterKnife;
import butterknife.OnClick;

public class LatencyTest extends Activity
        implements ActivityCompat.OnRequestPermissionsResultCallback {

    private static final String TAG = "LatencyTest";
    private static final double RESULT_MAX_DEV_RATIO = 0.15;
    private static final int RESULT_POOR_THRESHOLD = 40;
    private static final int RESULT_MEDIUM_THRESHOLD = 20;
    private static final String AVG_SD_FORMAT = "(%1$,.1f +/- %2$,.1f)";
    private static final int AUDIO_ECHO_REQUEST = 0;
    private static final int TEST_DURATION = 5000;
    private static final int TEST_RESULT_POLLING_PERIOD = 500;

    private static final String DATABASE_GROUP_RESULTS = "test_results";
    // Single out recording for run-permission needs
    private DatabaseReference databaseReference;
    private boolean created = false;

    @BindView(R.id.chart)
    LineChart chart;
    @BindView(R.id.summary_chart)
    BarChart summaryChart;

    @BindView(R.id.run_test_btn)
    Button button;

    private ProgressDialog dialog;

    @Override
    @TargetApi(17)
    protected void onCreate(Bundle icicle) {
        super.onCreate(icicle);
        setContentView(R.layout.main);
        ButterKnife.bind(this);
        initAudioFeatures();
        loadDataIntoChart(new long[1]);
        databaseReference = FirebaseDatabase.getInstance().getReference();
    }

    private void recordAudio() {
        if (!created) {
            created = createAudioRecorder();
        }
        if (created) {
            startRecording();
            startResultPollingThread();
        }
    }

    private void startResultPollingThread() {
        button.setEnabled(false);
        Thread t = new Thread(() -> {
            int reps = TEST_DURATION / TEST_RESULT_POLLING_PERIOD;
            long[] results = null;
            for (int i = 0; i < reps; i++) {
                SystemClock.sleep(TEST_RESULT_POLLING_PERIOD);
                results = checkResultBuffer();
                loadDataIntoChart(results);
            }
            if (results != null) {
                final long[] finalResults = results;
                runOnUiThread(() -> onTestFinished(finalResults));
            }
        });
        t.start();
    }

    private void loadDataIntoChart(long[] results) {
        List<Entry> chartEntries = new ArrayList<>();
        for (long result : results) {
            chartEntries.add(new Entry(chartEntries.size(), result));
        }
        LineDataSet dataSet = new LineDataSet(chartEntries, getString(R.string.latency));
        dataSet.setColor(Color.RED);
        final LineData chartData = new LineData(dataSet);
        Description desc = new Description();
        desc.setText("Partial latency results");
        runOnUiThread(() -> {
            chart.setDescription(desc);
            chart.setData(chartData);
            chart.invalidate();
        });
    }

    private void loadDataIntoSummaryChart(List<ModelTestResult> results) {
        List<BarEntry> chartEntries = new ArrayList<>();
        List<String> labels = new ArrayList<>();
        List<Integer> colors = new ArrayList<>();
        for (ModelTestResult result : results) {
            if (result.modelName.equals(getDeviceModelName()))
                colors.add(Color.argb(255, 150, 255, 125));
            else
                colors.add(Color.argb(255, 255, 0, (int) (255 * ((float) colors.size() / results.size()))));
            labels.add(result.modelName);
            chartEntries.add(new BarEntry(chartEntries.size(), getPartialAvg(result), result));
        }
        BarDataSet dataSet = new BarDataSet(chartEntries, getString(R.string.latency));
        dataSet.setLabel(null);
        dataSet.setStackLabels(labels.toArray(new String[0]));
        dataSet.setColors(colors);
        final BarData chartData = new BarData(dataSet);
        XAxis xAxis = summaryChart.getXAxis();
        summaryChart.getAxisRight().setEnabled(false);
        YAxis yAxis = summaryChart.getAxisLeft();
        xAxis.setValueFormatter((value, axis) ->
                value >= 0 && value < labels.size() ? labels.get((int) value) : "");
        xAxis.setGranularity(1f);
        yAxis.setAxisMinimum(0);
        yAxis.setGranularity(25f);
        Description desc = new Description();
        desc.setText("Results by model");
        runOnUiThread(() -> {
            summaryChart.setPinchZoom(false);
            summaryChart.setDescription(desc);
            summaryChart.setData(chartData);
            summaryChart.invalidate();
            try {
                dialog.dismiss();
            } catch (Exception e) {
                Log.e(TAG, e.getMessage());
            }
        });
    }

    private static float getPartialAvg(ModelTestResult result) {
        int count = 0;
        Double sum = 0.0;
        for (TestResult testResult : result.results.values()) {
            sum += testResult.avgLatency;
            count++;
        }
        if (count != 0)
            return BigDecimal.valueOf(sum / count).floatValue();
        return 0;
    }

    private void onTestFinished(long[] results) {
        TestResult avgSd = getAvgAndSD(results);
        if (isValidResult(avgSd)) {
            displayResultDialog(avgSd);
            saveResultToFirebase(avgSd);
        } else {
            displayInterruptedDialog();
        }
        showPreviousResults();
        button.setEnabled(true);
    }

    private static boolean isValidResult(TestResult testResult) {
        return testResult.stdDev / testResult.avgLatency < RESULT_MAX_DEV_RATIO;
    }

    private TestResult getAvgAndSD(long[] results) {
        double avg = 0;
        double sd = 0;
        int validResults = 0;
        for (long result : results) {
            if (result > 0) {
                avg += result;
                validResults++;
            }
        }
        if (validResults > 0) {
            avg /= validResults;
            for (long result : results) {
                if (result > 0)
                    sd += result * result - avg * avg;
            }
            sd = Math.sqrt(sd / validResults);
        } else {
            avg = -1;
            sd = -1;
        }
        return new TestResult(avg, sd);
    }

    private void displayResultDialog(TestResult avgSd) {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        int messageFormatId;
        if (avgSd.avgLatency > RESULT_POOR_THRESHOLD) {
            messageFormatId = R.string.dialog_message_poor;
        } else if (avgSd.avgLatency > RESULT_MEDIUM_THRESHOLD) {
            messageFormatId = R.string.dialog_message_medium;
        } else {
            messageFormatId = R.string.dialog_message_great;
        }
        builder.setTitle(R.string.dialog_title)
                .setMessage(String.format(getString(messageFormatId),
                        String.format(AVG_SD_FORMAT, avgSd.avgLatency, avgSd.stdDev)))
                .setPositiveButton(R.string.ok, null)
                .create()
                .show();
    }

    private void displayInterruptedDialog() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.dialog_title)
                .setMessage(getString(R.string.dialog_message_bad_conditions))
                .setPositiveButton(R.string.ok, null)
                .create()
                .show();
    }

    @Override
    protected void onDestroy() {
        shutdown();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        if (AUDIO_ECHO_REQUEST != requestCode) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }

        if (grantResults.length != 1 ||
                grantResults[0] != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(getApplicationContext(),
                    getString(R.string.NeedRecordAudioPermission),
                    Toast.LENGTH_SHORT)
                    .show();
            return;
        }
        recordAudio();
    }

    @OnClick(R.id.run_test_btn)
    public void record(View view) {
        switchCharts(true);
        int status = ActivityCompat.checkSelfPermission(LatencyTest.this,
                Manifest.permission.RECORD_AUDIO);
        if (status != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                    LatencyTest.this,
                    new String[]{Manifest.permission.RECORD_AUDIO},
                    AUDIO_ECHO_REQUEST);
            return;
        }
        recordAudio();
    }

    private void initAudioFeatures() {
        createEngine();

        int sampleRate = 0;
        int bufSize = 0;
        /*
         * retrieve fast audio path sample rate and buf size; if we have it, we pass to native
         * side to create a player with fast audio enabled [ fast audio == low latency audio ];
         * IF we do not have a fast audio path, we pass 0 for sampleRate, which will force native
         * side to pick up the 8Khz sample rate.
         */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1) {
            AudioManager myAudioMgr = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
            String nativeParam = myAudioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
            sampleRate = Integer.parseInt(nativeParam);
            nativeParam = myAudioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);
            bufSize = Integer.parseInt(nativeParam);
        }
        createBufferQueueAudioPlayer(sampleRate, bufSize);
    }

    private static String getDeviceModelName() {
        return Build.MANUFACTURER + " "
                + Build.MODEL + " "
                + Build.VERSION.RELEASE + " "
                + Build.VERSION_CODES.class.getFields()[android.os.Build.VERSION.SDK_INT].getName();
    }

    private static String getFirebaseModelKey() {
        return getDeviceModelName().replaceAll("[\\.#\\$\\[\\]]", "_");
    }

    private void saveResultToFirebase(TestResult result) {
        DatabaseReference ref = databaseReference.child(DATABASE_GROUP_RESULTS)
                .child(getFirebaseModelKey());
        ref.child("model_name").setValue(getDeviceModelName());
        ref.child("last_result").setValue(result.avgLatency);
        ref.child("results").push()
                .setValue(result,
                        (dbError, dbReference) -> {
                            if (dbError != null) {
                                Log.e(TAG, "Failed to save results", dbError.toException());
                            } else {
                                Log.e(TAG, "Saved successfully");
                            }
                        });
    }

    private ValueEventListener singleModelListener = new ValueEventListener() {
        @Override
        public void onDataChange(DataSnapshot dataSnapshot) {
            List<ModelTestResult> modelTestResults = new ArrayList<>();
            for (DataSnapshot childDataSnap : dataSnapshot.getChildren()) {
                modelTestResults.add(childDataSnap.getValue(ModelTestResult.class));
            }
            loadDataIntoSummaryChart(modelTestResults);
        }

        @Override
        public void onCancelled(DatabaseError databaseError) {
            Log.e(TAG, "Can't get previous results");
        }
    };

    private ProgressDialog showProgressDialog() {
        ProgressDialog dialog = new ProgressDialog(LatencyTest.this);
        dialog.setMessage("Loading results");
        dialog.show();
        return dialog;
    }

    private void showPreviousResults() {
        dialog = showProgressDialog();
        switchCharts(false);
        databaseReference.child(DATABASE_GROUP_RESULTS)
                .orderByChild("last_result")
                .addValueEventListener(singleModelListener);

    }

    private void switchCharts(boolean showCurrentTest) {
        chart.setVisibility(showCurrentTest ? View.VISIBLE : View.GONE);
        summaryChart.setVisibility(showCurrentTest ? View.GONE : View.VISIBLE);
    }

    /**
     * Native methods, implemented in jni folder
     */
    public static native void createEngine();

    public static native void createBufferQueueAudioPlayer(int sampleRate, int samplesPerBuf);

    public static native boolean createAudioRecorder();

    public static native void shutdown();

    public native void startRecording();

    public native long[] checkResultBuffer();

    /** Load jni .so on initialization */
    static {
        System.loadLibrary("native-audio-jni");
    }
}
