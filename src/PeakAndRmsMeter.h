/**********************************************************************

  Audacity: A Digital Audio Editor

  MeterPanel.h

  Dominic Mazzoni

  VU Meter, for displaying recording/playback level

  This is a bunch of common code that can display many different
  forms of VU meters and other displays.

**********************************************************************/

#ifndef __AUDACITY_METER_PANEL__
#define __AUDACITY_METER_PANEL__

#include <atomic>
#include <wx/setup.h> // for wxUSE_* macros
#include <wx/brush.h> // member variable
#include <wx/defs.h>
#include <wx/timer.h> // member variable

#include "ASlider.h"
#include "SampleFormat.h"
#include "Prefs.h"
#include "Meter.h"
#include "MeterPanelBase.h" // to inherit
#include "Observer.h"
#include "Ruler.h" // member variable

class AudacityProject;
struct AudioIOEvent;

// Increase this when we add support for multichannel meters
// (most of the code is already there)
const int kMaxMeterBars = 2;

struct MeterBar {
   bool   vert{};
   wxRect b{};         // Bevel around bar
   wxRect r{};         // True bar drawing area
   wxRect rClip{};
};

class MeterUpdateMsg
{
   public:
   int numFrames;
   float peak[kMaxMeterBars];
   float rms[kMaxMeterBars];
   bool clipping[kMaxMeterBars];
   int headPeakCount[kMaxMeterBars];
   int tailPeakCount[kMaxMeterBars];

   /* neither constructor nor destructor do anything */
   MeterUpdateMsg() { }
   ~MeterUpdateMsg() { }
   /* for debugging purposes, printing the values out is really handy */
   /** \brief Print out all the values in the meter update message */
   wxString toString();
   /** \brief Only print meter updates if clipping may be happening */
   wxString toStringIfClipped();
};

// Thread-safe queue of update messages
class MeterUpdateQueue
{
 public:
   explicit MeterUpdateQueue(size_t maxLen);
   ~MeterUpdateQueue();

   bool Put(MeterUpdateMsg &msg);
   bool Get(MeterUpdateMsg &msg);

   void Clear();

 private:
   // Align the two atomics to avoid false sharing
   // mStart is written only by the reader, mEnd by the writer
   NonInterfering< std::atomic<size_t> > mStart{ 0 }, mEnd{ 0 };

   const size_t mBufferSize;
   ArrayOf<MeterUpdateMsg> mBuffer{mBufferSize};
};

class MeterAx;

/*!
 This class uses a circular queue to communicate sample data from the
 low-latency audio thread to the main thread.  If the main thread
 does not consume frequently enough to leave sufficient empty space,
 extra data from the other thread is simply lost.
 */
class AUDACITY_DLL_API PeakAndRmsMeter
   : public Meter
   , public NonInterferingBase
{
public:
   struct Stats {
      void Reset(bool resetClipping)
      {
         peak = 0.0;
         rms = 0.0;
         peakHold = 0.0;
         peakHoldTime = 0.0;
         if (resetClipping) {
            clipping = false;
            peakPeakHold = 0.0;
         }
         tailPeakCount = 0;
      }
      float  peak{ 0 };
      float  rms{ 0 };
      float  peakHold{ 0 };
      double peakHoldTime{ 0 };
      bool   clipping{ false };
      int    tailPeakCount{ 0 };
      float  peakPeakHold{ 0 };
   };

   PeakAndRmsMeter(int dbRange,
      float decayRate = 60.0f // dB/sec
   );
   ~PeakAndRmsMeter() override;

   //! Call from the main thread to consume from the inter-thread queue
   /*!
    Updates the member mStats, to detect clipping, sufficiently longheld peak,
    and a trailing exponential moving average of the RMS signal, which may be
    used in drawing
    */
   void Poll();

   //! Receive one message corresponding to given time
   /*!
    Default implementation does nothing
    @param time clock time relative to last Reset()
    @param msg its `peak` and `rms` adjusted to dB when `mdB`
    */
   virtual void Receive(double time, const MeterUpdateMsg &msg);

   void Clear() override;
   void Reset(double sampleRate, bool resetClipping) override;

   //! Update the meters with a block of audio data
   /*!
    Process the supplied block of audio data, extracting the peak and RMS
    levels to send to the meter. Also record runs of clipped samples to detect
    clipping that lies on block boundaries.
    This method is thread-safe!  Feel free to call from a different thread
    (like from an audio I/O callback).

    @param numChannels The number of channels of audio being played back or
    recorded.
    @param numFrames The number of frames (samples) in this data block. It is
    assumed that there are the same number of frames in each channel.
    @param sampleData The audio data itself.
    */
   void Update(unsigned numChannels,
      unsigned long numFrames, const float *sampleData, bool interleaved)
   override;

   //! Find out if the level meter is disabled or not.
   /*!
    This method is thread-safe!  Feel free to call from a
    different thread (like from an audio I/O callback).
    */
   bool IsDisabled() const override;

   bool IsClipping() const;
   int GetDBRange() const;

protected:
   MeterUpdateQueue mQueue{ 1024 };
   float     mDecayRate{}; // dB/sec
   unsigned  mNumBars{ 0 };
   Stats  mStats[kMaxMeterBars]{};
   int mNumPeakSamplesToClip{ 3 };
   int       mDBRange{ 60 };
   bool      mDB{ true };
   bool mMeterDisabled{};

private:
   double mRate{};
   double mT{};
   int mPeakHoldDuration{ 3 };
   bool mDecay{ true };
};

/********************************************************************//**
\brief MeterPanel is a panel that paints the meter used for monitoring
or playback.
************************************************************************/
class AUDACITY_DLL_API MeterPanel final
   : public MeterPanelBase
   , public PeakAndRmsMeter
   , private PrefsListener
{
   DECLARE_DYNAMIC_CLASS(MeterPanel)

 public:
   // These should be kept in the same order as they appear
   // in the menu
   enum Style {
      AutomaticStereo,
      HorizontalStereo,
      VerticalStereo,
      MixerTrackCluster, // Doesn't show menu, icon, or L/R labels, but otherwise like VerticalStereo.
      HorizontalStereoCompact, // Thinner.
      VerticalStereoCompact, // Narrower.
   };

   MeterPanel(AudacityProject *,
         wxWindow* parent, wxWindowID id,
         bool isInput,
         const wxPoint& pos = wxDefaultPosition,
         const wxSize& size = wxDefaultSize,
         Style style = HorizontalStereo);

   void SetFocusFromKbd() override;

   Style GetStyle() const { return mStyle; }
   Style GetDesiredStyle() const { return mDesiredStyle; }
   void SetStyle(Style newStyle);

   /** \brief
    *
    * This method is thread-safe!  Feel free to call from a
    * different thread (like from an audio I/O callback).
    */
   void Reset(double sampleRate, bool resetClipping) override;

   void Receive(double time, const MeterUpdateMsg &msg) override;

   // Vaughan, 2010-11-29: This not currently used. See comments in MixerTrackCluster::UpdateMeter().
   //void UpdateDisplay(int numChannels, int numFrames,
   //                     // Need to make these double-indexed max and min arrays if we handle more than 2 channels.
   //                     float* maxLeft, float* rmsLeft,
   //                     float* maxRight, float* rmsRight,
   //                     const size_t kSampleCount);

   float GetPeakHold() const;

   bool IsMonitoring() const;
   bool IsActive() const;

   void StartMonitoring();
   void StopMonitoring();

   // These exist solely for the purpose of resetting the toolbars
   struct State{ bool mSaved, mMonitoring, mActive; };
   State SaveState();
   void RestoreState(const State &state);
   void SetMixer(wxCommandEvent& event);

   bool ShowDialog();
   void Increase(float steps);
   void Decrease(float steps);
   void UpdateSliderControl();
   
   void ShowMenu(const wxPoint & pos);

   void SetName(const TranslatableString& name);

 private:
   void UpdatePrefs() override;
   void UpdateSelectedPrefs( int ) override;

 private:
   //
   // Event handlers
   //
   void OnErase(wxEraseEvent &evt);
   void OnPaint(wxPaintEvent &evt);
   void OnSize(wxSizeEvent &evt);
   void OnMouse(wxMouseEvent &evt);
   void OnKeyDown(wxKeyEvent &evt);
   void OnCharHook(wxKeyEvent &evt);
   void OnContext(wxContextMenuEvent &evt);
   void OnSetFocus(wxFocusEvent &evt);
   void OnKillFocus(wxFocusEvent &evt);

   void OnAudioIOStatus(const AudioIOEvent &event);
   void OnAudioCapture(const AudioIOEvent &event);

   void OnMeterUpdate(wxTimerEvent &evt);
   void OnTipTimeout(wxTimerEvent& evt);

   void HandleLayout(wxDC &dc);
   void SetActiveStyle(Style style);
   void SetBarAndClip(int iBar, bool vert);
   void DrawMeterBar(wxDC &dc, MeterBar &meterBar, Stats &stats);
   void RepaintBarsNow();
   wxFont GetFont() const;

   //
   // Pop-up menu
   //
   void OnMonitor(wxCommandEvent &evt);
   void OnPreferences(wxCommandEvent &evt);

   wxString Key(const wxString & key) const;

   Observer::Subscription mAudioIOStatusSubscription;
   Observer::Subscription mAudioCaptureSubscription;

   AudacityProject *mProject;
   wxTimer          mTimer;
   wxTimer          mTipTimer;

   int       mWidth;
   int       mHeight;

   int       mRulerWidth{};
   int       mRulerHeight{};

   bool      mIsInput;

   Style     mStyle{};
   Style     mDesiredStyle;
   bool      mGradient;
   bool      mClip;
   int       mNumPeakSamplesToClip;
   double    mPeakHoldDuration;
   double    mRate;
   long      mMeterRefreshRate{};

   bool      mMonitoring;

   bool      mActive;

   MeterBar  mBar[kMaxMeterBars]{};

   bool      mLayoutValid;

   std::unique_ptr<wxBitmap> mBitmap;
   wxPoint   mLeftTextPos;
   wxPoint   mRightTextPos;
   wxSize    mLeftSize;
   wxSize    mRightSize;
   wxPen     mPen;
   wxPen     mDisabledPen;
   wxPen     mPeakPeakPen;
   wxBrush   mBrush;
   wxBrush   mRMSBrush;
   wxBrush   mClipBrush;
   wxBrush   mBkgndBrush;
   wxBrush   mDisabledBkgndBrush;
   Ruler     mRuler;
   wxString  mLeftText;
   wxString  mRightText;

   std::unique_ptr<LWSlider> mSlider;
   wxPoint mSliderPos;
   wxSize mSliderSize;

   bool mEnabled{ true };

   bool mIsFocused{};
   wxRect mFocusRect;

   /*! @name state variables during OnMeterUpdate
     @{
    */
   double mMaxPeak{};
   unsigned mNumChanges{};
   bool mDiscarded{};
   /*!
     @}
    */

   friend class MeterAx;

   DECLARE_EVENT_TABLE()
};

#endif // __AUDACITY_METER_PANEL__
