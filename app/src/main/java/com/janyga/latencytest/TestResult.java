package com.janyga.latencytest;

import com.google.firebase.database.IgnoreExtraProperties;

@IgnoreExtraProperties
public class TestResult {
    public Double avgLatency;
    public Double stdDev;

    public TestResult() {
        //required by firebase
    }

    public TestResult(Double avgLatency, Double stdDev) {
        this.avgLatency = avgLatency;
        this.stdDev = stdDev;
    }
}
