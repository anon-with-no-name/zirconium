// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PLUGINS_RENDERER_MOBILE_YOUTUBE_PLUGIN_H_
#define COMPONENTS_PLUGINS_RENDERER_MOBILE_YOUTUBE_PLUGIN_H_

#include "components/plugins/renderer/plugin_placeholder.h"

namespace plugins {

// Class representing placeholders for old style embedded youtube video on
// mobile device. For old style embedded youtube video, it has a url in the form
// of http://www.youtube.com/v/VIDEO_ID. This placeholder replaces the url with
// a simple html page and clicking the play image redirects the user to the
// mobile youtube app.
class MobileYouTubePlugin : public PluginPlaceholder {
 public:
  MobileYouTubePlugin(content::RenderFrame* render_frame,
                      blink::WebLocalFrame* frame,
                      const blink::WebPluginParams& params,
                      base::StringPiece& template_html,
                      GURL placeholderDataUrl);

  // Whether this is a youtube url.
  static bool IsYouTubeURL(const GURL& url, const std::string& mime_type);

 private:
  ~MobileYouTubePlugin() override;

  // Opens a youtube app in the current tab.
  void OpenYoutubeUrlCallback();

  // WebViewPlugin::Delegate (via PluginPlaceholder) method
  void BindWebFrame(blink::WebFrame* frame) override;

  // gin::Wrappable (via PluginPlaceholder) method
  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;

  DISALLOW_COPY_AND_ASSIGN(MobileYouTubePlugin);
};

}  // namespace plugins

#endif  // COMPONENTS_PLUGINS_RENDERER_MOBILE_YOUTUBE_PLUGIN_H_