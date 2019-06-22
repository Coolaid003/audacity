/**********************************************************************

  Audacity: A Digital Audio Editor

  TrackPanel.h

  Dominic Mazzoni

**********************************************************************/

#ifndef __AUDACITY_TRACK_PANEL__
#define __AUDACITY_TRACK_PANEL__

#include "Audacity.h" // for USE_* macros
#include "Experimental.h"

#include <vector>

#include <wx/setup.h> // for wxUSE_* macros
#include <wx/timer.h> // to inherit

#include "HitTestResult.h"
#include "Prefs.h"

#include "SelectedRegion.h"

#include "CellularPanel.h"

#include "commands/CommandManagerWindowClasses.h"


class wxRect;

class LabelTrack;
class SpectrumAnalyst;
class Track;
class TrackList;
struct TrackListEvent;
class TrackPanel;
class TrackArtist;
class Ruler;
class SnapManager;
class AdornedRulerPanel;
class LWSlider;

class TrackPanelAx;

class NoteTrack;
class WaveTrack;
class WaveClip;

// Declared elsewhere, to reduce compilation dependencies
class TrackPanelListener;

struct TrackPanelDrawingContext;

enum class UndoPush : unsigned char;

enum {
   kTimerInterval = 50, // milliseconds
};

const int DragThreshold = 3;// Anything over 3 pixels is a drag, else a click.

class AUDACITY_DLL_API TrackPanel final
   : public CellularPanel
   , public NonKeystrokeInterceptingWindow
   , private PrefsListener
{
 public:
   static TrackPanel &Get( AudacityProject &project );
   static const TrackPanel &Get( const AudacityProject &project );
   static void Destroy( AudacityProject &project );
 
   TrackPanel(wxWindow * parent,
              wxWindowID id,
              const wxPoint & pos,
              const wxSize & size,
              const std::shared_ptr<TrackList> &tracks,
              ViewInfo * viewInfo,
              AudacityProject * project,
              AdornedRulerPanel * ruler );

   virtual ~ TrackPanel();

   void UpdatePrefs() override;

   void OnPaint(wxPaintEvent & event);
   void OnMouseEvent(wxMouseEvent & event);
   void OnKeyDown(wxKeyEvent & event);

   void OnTrackListResizing(TrackListEvent & event);
   void OnTrackListDeletion(wxEvent & event);
   void UpdateViewIfNoTracks(); // Call this to update mViewInfo, etc, after track(s) removal, before Refresh().

   double GetMostRecentXPos();

   void OnIdle(wxIdleEvent & event);
   void OnTimer(wxTimerEvent& event);
   void OnODTask(wxCommandEvent &event);
   void OnProjectSettingsChange(wxCommandEvent &event);

   int GetLeftOffset() const { return GetLabelWidth() + 1;}

   wxSize GetTracksUsableArea() const;

   // Width and height, relative to upper left corner at (GetLeftOffset(), 0)
   // Either argument may be NULL
   void GetTracksUsableArea(int *width, int *height) const;

   void OnUndoReset( wxCommandEvent &event );

   void Refresh
      (bool eraseBackground = true, const wxRect *rect = (const wxRect *) NULL)
      override;

   void RefreshTrack(Track *trk, bool refreshbacking = true);

   void DisplaySelection();

   void HandlePageUpKey();
   void HandlePageDownKey();
   AudacityProject * GetProject() const override;

   void ScrollIntoView(double pos);
   void ScrollIntoView(int x);

   void OnTrackMenu(Track *t = NULL);
   Track * GetFirstSelectedTrack();

   void EnsureVisible(Track * t);
   void VerticalScroll( float fracPosition);

   TrackPanelCell *GetFocusedCell() override;
   void SetFocusedCell() override;
   Track *GetFocusedTrack();
   void SetFocusedTrack(Track *t);

   void UpdateVRulers();
   void UpdateVRuler(Track *t);
   void UpdateTrackVRuler(const Track *t);
   void UpdateVRulerSize();

   // Returns the time corresponding to the pixel column one past the track area
   // (ignoring any fisheye)
   double GetScreenEndTime() const;

 protected:
   bool IsAudioActive();

public:
   size_t GetTrackCount() const;
   size_t GetSelectedTrackCount() const;

protected:
   void UpdateSelectionDisplay();

public:
   void UpdateAccessibility();
   void MessageForScreenReader(const wxString& message);

   void MakeParentRedrawScrollbars();

   // Rectangle includes track control panel, and the vertical ruler, and
   // the proper track area of all channels, and the separators between them.
   wxRect FindTrackRect( const Track * target );

protected:
   void MakeParentModifyState(bool bWantsAutoSave);    // if true, writes auto-save file. Should set only if you really want the state change restored after
                                                               // a crash, as it can take many seconds for large (eg. 10 track-hours) projects

   // Get the root object defining a recursive subdivision of the panel's
   // area into cells
   std::shared_ptr<TrackPanelNode> Root() override;

   int GetVRulerWidth() const;

public:
   int GetLabelWidth() const;

// JKC Nov-2011: These four functions only used from within a dll such as mod-track-panel
// They work around some messy problems with constructors.
   const TrackList * GetTracks() const { return mTracks.get(); }
   TrackList * GetTracks() { return mTracks.get(); }
   ViewInfo * GetViewInfo(){ return mViewInfo;}
   TrackPanelListener * GetListener(){ return mListener;}
   AdornedRulerPanel * GetRuler(){ return mRuler;}

protected:
   void DrawTracks(wxDC * dc);

   void DrawEverythingElse(TrackPanelDrawingContext &context,
                           const wxRegion & region,
                           const wxRect & clip);
   void DrawOutside(
      TrackPanelDrawingContext &context,
      const Track *leaderTrack, const wxRect & teamRect);

   void HighlightFocusedTrack (wxDC* dc, const wxRect &rect);
   void DrawShadow            ( wxDC* dc, const wxRect & rect );
   void DrawBordersAroundTrack(wxDC* dc, const wxRect & rect );
   void ClearTopMargin        (
      TrackPanelDrawingContext &context, const wxRect &clip);
   void ClearLeftAndRightMargins    (
      TrackPanelDrawingContext &context, const wxRect & clip);
   void ClearSeparator    (
      TrackPanelDrawingContext &context, const wxRect & rect);
   void DrawSash              (
      wxDC* dc, const wxRect & rect, int labelw, bool bSelected );

public:
   // Set the object that performs catch-all event handling when the pointer
   // is not in any track or ruler or control panel.
   void SetBackgroundCell
      (const std::shared_ptr< TrackPanelCell > &pCell);
   std::shared_ptr< TrackPanelCell > GetBackgroundCell();

public:

protected:
   TrackPanelListener *mListener;

   std::shared_ptr<TrackList> mTracks;

   AdornedRulerPanel *mRuler;

   std::unique_ptr<TrackArtist> mTrackArtist;

   class AUDACITY_DLL_API AudacityTimer final : public wxTimer {
   public:
     void Notify() override{
       // (From Debian)
       //
       // Don't call parent->OnTimer(..) directly here, but instead post
       // an event. This ensures that this is a pure wxWidgets event
       // (no GDK event behind it) and that it therefore isn't processed
       // within the YieldFor(..) of the clipboard operations (workaround
       // for Debian bug #765341).
       // QueueEvent() will take ownership of the event
       parent->GetEventHandler()->QueueEvent(safenew wxTimerEvent(*this));
     }
     TrackPanel *parent;
   } mTimer;

   int mTimeCount;

   bool mRefreshBacking;

#ifdef EXPERIMENTAL_SPECTRAL_EDITING

protected:

#endif

   bool mRedrawAfterStop;

   friend class TrackPanelAx;

#if wxUSE_ACCESSIBILITY
   TrackPanelAx *mAx{};
#else
   std::unique_ptr<TrackPanelAx> mAx;
#endif

public:
   TrackPanelAx &GetAx() { return *mAx; }

protected:

   // The screenshot class needs to access internals
   friend class ScreenshotCommand;

   SelectedRegion mLastDrawnSelectedRegion {};

 public:
   wxSize vrulerSize;

 protected:

   std::shared_ptr<TrackPanelCell> mpBackground;

   DECLARE_EVENT_TABLE()

   void ProcessUIHandleResult
      (TrackPanelCell *pClickedTrack, TrackPanelCell *pLatestCell,
       unsigned refreshResult) override;

   void UpdateStatusMessage( const wxString &status ) override;

   // friending GetInfoCommand allow automation to get sizes of the
   // tracks, track control panel and such.
   friend class GetInfoCommand;
};

// A predicate class
struct IsVisibleTrack
{
   IsVisibleTrack(AudacityProject *project);

   bool operator () (const Track *pTrack) const;

   wxRect mPanelRect;
};

#endif
