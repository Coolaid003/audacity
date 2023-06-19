/**********************************************************************

Audacity: A Digital Audio Editor

ChannelView.h

Paul Licameli split from class Track

**********************************************************************/

#ifndef __AUDACITY_TRACK_VIEW__
#define __AUDACITY_TRACK_VIEW__

#include <memory>
#include "CommonTrackPanelCell.h" // to inherit
#include "XMLAttributeValueView.h"

class Channel;
class Track;
class TrackList;
class TrackVRulerControls;
class TrackPanelResizerCell;

class AUDACITY_DLL_API ChannelView /* not final */ : public CommonTrackCell
   , public std::enable_shared_from_this<ChannelView>
{
   ChannelView(const ChannelView&) = delete;
   ChannelView &operator=(const ChannelView&) = delete;

public:
   enum : unsigned { DefaultHeight = 150 };

   static ChannelView &Get(Channel &channel);
   /*!
    @copydoc Get(Channel &)
    */
   static const ChannelView &Get(const Channel &channel);
   static ChannelView *Find(Channel *pChannel);
   /*!
    @copydoc Find(Track *)
    */
   static const ChannelView *Find(const Channel *pChannel);

   //! Construct from a track and a channel index
   ChannelView(const std::shared_ptr<Track> &pTrack, size_t iChannel);
   virtual ~ChannelView() = 0;

   // some static conveniences, useful for summation over track iterator
   // ranges
   static int GetChannelGroupHeight(const Track *pTrack);
   // Total height of the given channel and all previous ones (constant time!)
   static int GetCumulativeHeight(const Channel *pChannel);
   // Total height of all Channels of the the given track and all previous ones
   // (constant time!)
   static int GetCumulativeHeight(const Track *pTrack);
   static int GetTotalHeight(const TrackList &list);

   // Copy view state, for undo/redo purposes
   void CopyTo(Track &track) const override;

   bool GetMinimized() const { return mMinimized; }
   void SetMinimized( bool minimized );

   //! @return cached sum of `GetHeight()` of all preceding tracks
   int GetCumulativeHeightBefore() const { return mY; }

   //! @return height of the track when expanded
   /*! See other comments for GetHeight */
   int GetExpandedHeight() const { return mHeight; }

   //! @return height of the track when collapsed
   /*! See other comments for GetHeight */
   virtual int GetMinimizedHeight() const = 0;

   //! @return height of the track as it now appears, expanded or collapsed
   /*!
    Total "height" of channels of a track includes padding areas above and
    below it, and is pixel-accurate for the channel group.
    The "heights" of channels within a group determine the proportions of
    heights of the track data shown -- but the actual total pixel heights
    may differ when other fixed-height adornments and paddings are added,
    according to other rules for allocation of height.
   */
   int GetHeight() const;

   //! Set cached value dependent on position within the track list
   void SetCumulativeHeightBefore(int y) { DoSetY( y ); }

   /*! Sets height for expanded state.
    Does not expand a track if it is now collapsed.
    See other comments for GetHeight
    */
   void SetExpandedHeight(int height);

   // Return another, associated TrackPanelCell object that implements the
   // mouse actions for the vertical ruler
   std::shared_ptr<TrackVRulerControls> GetVRulerControls();
   std::shared_ptr<const TrackVRulerControls> GetVRulerControls() const;

   // Returns cell that would be used at affordance area, by default returns nullptr,
   // meaning that track has no such area.
   virtual std::shared_ptr<CommonTrackCell> GetAffordanceControls();

   void WriteXMLAttributes( XMLWriter & ) const override;
   bool HandleXMLAttribute(
      const std::string_view& attr, const XMLAttributeValueView& valueView )
   override;

   // New virtual function.  The default just returns a one-element array
   // containing this.  Overrides might refine the Y axis.
   using Refinement = std::vector<
      std::pair<wxCoord, std::shared_ptr<ChannelView>>
   >;
   virtual Refinement GetSubViews( const wxRect &rect );

   // default is false
   virtual bool IsSpectral() const;

   virtual void DoSetMinimized( bool isMinimized );

private:

   // No need yet to make this virtual
   void DoSetY(int y);

   void DoSetHeight(int h);

protected:

   // Private factory to make appropriate object; class ChannelView handles
   // memory management thereafter
   virtual std::shared_ptr<TrackVRulerControls> DoGetVRulerControls() = 0;

   std::shared_ptr<TrackVRulerControls> mpVRulerControls;

private:
   /*!
    @pre `iChannel < track.NChannels()`
    */
   static ChannelView &GetFromTrack(Track &track, size_t iChannel = 0);
   /*!
    @copydoc Get(Track&, size_t)
    */
   static const ChannelView &GetFromTrack(const Track &track, size_t iChannel = 0);
   /*!
    @pre `!pTrack || iChannel < pTrack->NChannels()`
    */
   static ChannelView *FindFromTrack(Track *pTrack, size_t iChannel = 0);

   bool           mMinimized{ false };
   int            mY{ 0 };
   int            mHeight{ DefaultHeight };
};

#include "AttachedVirtualFunction.h"

struct DoGetViewTag;

//! Declare an open method to get the view object associated with a track
/*!
 @pre The channel index argument is less than the track's `NChannels()`
 */
using DoGetView =
AttachedVirtualFunction<
   DoGetViewTag,
   std::shared_ptr<ChannelView>,
   Track,
   size_t// channel index
>;
DECLARE_EXPORTED_ATTACHED_VIRTUAL(AUDACITY_DLL_API, DoGetView);

struct GetDefaultTrackHeightTag;

using GetDefaultTrackHeight =
AttachedVirtualFunction<
   GetDefaultTrackHeightTag,
   int,
   Track
>;
DECLARE_EXPORTED_ATTACHED_VIRTUAL(AUDACITY_DLL_API, GetDefaultTrackHeight);

#endif