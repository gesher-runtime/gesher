/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.view;

import android.graphics.Matrix;
import android.graphics.Outline;

/**
 * Compatibility stand-in for android.view.RenderNode (pre-API 29),
 * delegating to android.graphics.RenderNode.
 */
public final class RenderNode {
    private final android.graphics.RenderNode mNode;

    private RenderNode(String name) {
        mNode = new android.graphics.RenderNode(name);
    }

    public static RenderNode create(String name, View ownerView) {
        return new RenderNode(name);
    }

    public boolean setAlpha(float alpha) {
        return mNode.setAlpha(alpha);
    }

    public float getAlpha() {
        return mNode.getAlpha();
    }

    public boolean setElevation(float elevation) {
        return mNode.setElevation(elevation);
    }

    public float getElevation() {
        return mNode.getElevation();
    }

    public boolean setTranslationX(float translationX) {
        return mNode.setTranslationX(translationX);
    }

    public boolean setTranslationY(float translationY) {
        return mNode.setTranslationY(translationY);
    }

    public boolean setTranslationZ(float translationZ) {
        return mNode.setTranslationZ(translationZ);
    }

    public boolean setRotation(float rotation) {
        return mNode.setRotationZ(rotation);
    }

    public boolean setRotationX(float rotationX) {
        return mNode.setRotationX(rotationX);
    }

    public boolean setRotationY(float rotationY) {
        return mNode.setRotationY(rotationY);
    }

    public boolean setScaleX(float scaleX) {
        return mNode.setScaleX(scaleX);
    }

    public boolean setScaleY(float scaleY) {
        return mNode.setScaleY(scaleY);
    }

    public boolean setPivotX(float pivotX) {
        return mNode.setPivotX(pivotX);
    }

    public boolean setPivotY(float pivotY) {
        return mNode.setPivotY(pivotY);
    }

    public boolean setCameraDistance(float distance) {
        return mNode.setCameraDistance(distance);
    }

    public boolean setLeftTopRightBottom(int left, int top, int right, int bottom) {
        return mNode.setPosition(left, top, right, bottom);
    }

    public boolean offsetLeftAndRight(int offset) {
        return mNode.offsetLeftAndRight(offset);
    }

    public boolean offsetTopAndBottom(int offset) {
        return mNode.offsetTopAndBottom(offset);
    }

    public boolean setClipToBounds(boolean clipToBounds) {
        return mNode.setClipToBounds(clipToBounds);
    }

    public boolean setProjectBackwards(boolean projectBackwards) {
        return mNode.setProjectBackwards(projectBackwards);
    }

    public boolean setProjectionReceiver(boolean shouldReceive) {
        return mNode.setProjectionReceiver(shouldReceive);
    }

    public boolean setOutline(Outline outline) {
        return mNode.setOutline(outline);
    }

    public boolean hasIdentityMatrix() {
        return mNode.hasIdentityMatrix();
    }

    public void getMatrix(Matrix outMatrix) {
        mNode.getMatrix(outMatrix);
    }

    public void getInverseMatrix(Matrix outMatrix) {
        mNode.getInverseMatrix(outMatrix);
    }

    public boolean hasDisplayList() {
        return mNode.hasDisplayList();
    }

    public void discardDisplayList() {
        mNode.discardDisplayList();
    }

    public DisplayListCanvas start(int width, int height) {
        return new DisplayListCanvas(mNode.beginRecording(width, height));
    }

    public void end(DisplayListCanvas canvas) {
        mNode.endRecording();
    }
}
