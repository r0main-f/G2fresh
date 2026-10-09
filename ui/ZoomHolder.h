// Shows a component scaled by a zoom factor inside a Viewport: the holder is
// sized to the scaled content so the scroll range follows the zoom.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace g2ui {

class ZoomHolder : public juce::Component, private juce::ComponentListener {
public:
    explicit ZoomHolder(juce::Component& content) : content_(content)
    {
        addAndMakeVisible(content_);
        content_.addComponentListener(this);
        update();
    }

    ~ZoomHolder() override { content_.removeComponentListener(this); }

    void setZoom(float zoom)
    {
        zoom_ = zoom;
        content_.setTransform(juce::AffineTransform::scale(zoom));
        update();
    }
    float zoom() const { return zoom_; }

private:
    void componentMovedOrResized(juce::Component&, bool, bool wasResized) override
    {
        if (wasResized)
            update();
    }

    void update()
    {
        content_.setTopLeftPosition(0, 0);
        setSize(juce::roundToInt(static_cast<float>(content_.getWidth()) * zoom_),
                juce::roundToInt(static_cast<float>(content_.getHeight()) * zoom_));
    }

    juce::Component& content_;
    float zoom_ = 1.0f;
};

} // namespace g2ui
