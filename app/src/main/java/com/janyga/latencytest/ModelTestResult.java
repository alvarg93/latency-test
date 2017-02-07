package com.janyga.latencytest;

import com.google.firebase.database.IgnoreExtraProperties;
import com.google.firebase.database.PropertyName;

import java.util.Map;

@IgnoreExtraProperties
public class ModelTestResult {

    @PropertyName("model_name")
    public String modelName;
    @PropertyName("last_result")
    public Double lastResult;
    public Map<String, TestResult> results;

    public ModelTestResult() {
        //required by firebase
    }

    public ModelTestResult(String modelName, Map<String, TestResult> results) {
        this.modelName = modelName;
        this.results = results;
    }
}
