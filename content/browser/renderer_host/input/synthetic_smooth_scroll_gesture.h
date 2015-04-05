// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_SYNTHETIC_SMOOTH_SCROLL_GESTURE_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_SYNTHETIC_SMOOTH_SCROLL_GESTURE_H_

#include "content/browser/renderer_host/input/synthetic_smooth_move_gesture.h"

#include "content/common/input/synthetic_smooth_scroll_gesture_params.h"

namespace content {
class CONTENT_EXPORT SyntheticSmoothScrollGesture : public SyntheticGesture {
 public:
  explicit SyntheticSmoothScrollGesture(
      const SyntheticSmoothScrollGestureParams& params);
  ~SyntheticSmoothScrollGesture() override;

  // SyntheticGesture implementation:
  SyntheticGesture::Result ForwardInputEvents(
      const base::TimeTicks& timestamp,
      SyntheticGestureTarget* target) override;

 private:
  static SyntheticSmoothMoveGestureParams::InputType GetInputSourceType(
      SyntheticGestureParams::GestureSourceType gesture_source_type);

  bool InitializeMoveGesture(
      SyntheticGestureParams::GestureSourceType gesture_type,
      SyntheticGestureTarget* target);

  scoped_ptr<SyntheticSmoothMoveGesture> move_gesture_;
  SyntheticSmoothScrollGestureParams params_;
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_SYNTHETIC_SMOOTH_SCROLL_GESTURE_H_