/**********************************************************************

Audacity: A Digital Audio Editor

TrackView.h

Paul Licameli split from class Track

**********************************************************************/

#ifndef __AUDACITY_TRACK_VIEW__
#define __AUDACITY_TRACK_VIEW__

#include <memory>
#include "CommonTrackPanelCell.h" // to inherit

class Track;
class TrackVRulerControls;
class TrackPanelResizerCell;

class TrackView /* not final */ : public CommonTrackCell
   , public std::enable_shared_from_this<TrackView>
{
   TrackView( const TrackView& ) = delete;
   TrackView &operator=( const TrackView& ) = delete;

public:
   enum : unsigned { DefaultHeight = 150 };

   explicit
   TrackView( const std::shared_ptr<Track> &pTrack )
      : CommonTrackCell{ pTrack } {}
   virtual ~TrackView() = 0;

   // Copy view state, for undo/redo purposes
   virtual void Copy( const TrackView &other );

   static TrackView &Get( Track & );
   static const TrackView &Get( const Track & );

   bool GetMinimized() const { return mMinimized; }
   void SetMinimized( bool minimized );

   int GetY() const { return mY; }
   int GetActualHeight() const { return mHeight; }
   virtual int GetMinimizedHeight() const = 0;
   int GetHeight() const;

   void SetY(int y) { DoSetY( y ); }
   void SetHeight(int height);

   // Return another, associated TrackPanelCell object that implements the
   // mouse actions for the vertical ruler
   std::shared_ptr<TrackVRulerControls> GetVRulerControls();
   std::shared_ptr<const TrackVRulerControls> GetVRulerControls() const;


   // Return another, associated TrackPanelCell object that implements the
   // click and drag to resize
   std::shared_ptr<TrackPanelCell> GetResizer();
   std::shared_ptr<const TrackPanelCell> GetResizer() const;

protected:
   virtual void DoSetMinimized( bool isMinimized );

   // No need yet to make this virtual
   void DoSetY(int y);

   virtual void DoSetHeight(int h);

   // Private factory to make appropriate object; class TrackView handles
   // memory management thereafter
   virtual std::shared_ptr<TrackVRulerControls> DoGetVRulerControls() = 0;

   std::shared_ptr<TrackVRulerControls> mpVRulerControls;
   std::shared_ptr<TrackPanelResizerCell> mpResizer;

private:
   bool           mMinimized{ false };
   int            mY{ 0 };
   int            mHeight{ DefaultHeight };
};

#endif
