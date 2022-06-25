/**********************************************************************

  Audacity: A Digital Audio Editor

  LV2Ports.cpp

  Paul Licameli split from LV2Effect.cpp

  Audacity(R) is copyright (c) 1999-2013 Audacity Team.
  License: GPL v2 or later.  See License.txt.

*********************************************************************/

#include "LV2Ports.h"
#include "LV2Symbols.h"
#include "Internat.h"

#include <cmath>

size_t LV2ControlPort::Discretize(float value) const
{
   auto s = mScaleValues.size();
   for (; s > 0 && --s;)
      if (value >= mScaleValues[s])
         break;
   return s;
}

LV2Ports::LV2Ports(const LilvPlugin &plug)
{
   using namespace LV2Symbols;

   // Collect information in mAudioPorts, mControlPorts, mAtomPorts, mCVPorts
   // Retrieve the port ranges for all ports (some values may be NaN)
   auto numPorts = lilv_plugin_get_num_ports(&plug);
   Floats minimumVals {numPorts};
   Floats maximumVals {numPorts};
   Floats defaultVals {numPorts};
   lilv_plugin_get_port_ranges_float(&plug,
      minimumVals.get(), maximumVals.get(), defaultVals.get());

   for (size_t i = 0; i < numPorts; ++i) {
      const auto port = lilv_plugin_get_port_by_index(&plug, i);
      int index = lilv_port_get_index(&plug, port);

      // It must be input or output, anything else is bogus
      bool isInput;
      if (lilv_port_is_a(&plug, port, node_InputPort))
         isInput = true;
      else if (lilv_port_is_a(&plug, port, node_OutputPort))
         isInput = false;
      else {
         assert(false);
         continue;
      }

      // Get the port name and symbol
      const auto symbol = LilvString(lilv_port_get_symbol(&plug, port));
      const auto name = LilvStringMove(lilv_port_get_name(&plug, port));

      // Get the group to which this port belongs or default to the main group
      TranslatableString groupName{};
      if (LilvNodePtr group{ lilv_port_get(&plug, port, node_Group) }) {
         // lilv.h does not say whether return of lilv_world_get() needs to
         // be freed, but that is easily seen to be so from source
         auto groupMsg = LilvStringMove(
            lilv_world_get(gWorld, group.get(), node_Label, nullptr));
         if (groupMsg.empty())
            groupMsg = LilvStringMove(
               lilv_world_get(gWorld, group.get(), node_Name, nullptr));
         if (groupMsg.empty())
            groupMsg = LilvString(group.get());
         groupName = Verbatim(groupMsg);
      }
      else
         groupName = XO("Effect Settings");

      // Get the latency port
      const auto latencyIndex = lilv_plugin_get_latency_port_index(&plug);

      // Get the ports designation (must be freed)
      LilvNodePtr designation{ lilv_port_get(&plug, port, node_Designation) };

      // Check for audio ports
      if (lilv_port_is_a(&plug, port, node_AudioPort)) {
         mAudioPorts.push_back(std::make_shared<LV2AudioPort>(
            port, index, isInput, symbol, name, groupName));
         isInput ? mAudioIn++ : mAudioOut++;
      }
      // Check for Control ports
      else if (lilv_port_is_a(&plug, port, node_ControlPort)) {
         // Add group if not previously done...
         if (mGroupMap.find(groupName) == mGroupMap.end())
            mGroups.push_back(groupName);
         // ... That maintains the postcondition, after this:
         mGroupMap[groupName].push_back(mControlPorts.size());

         wxString units;
         // Get any unit descriptor
         if (LilvNodePtr unit{ lilv_port_get(&plug, port, node_Unit) })
            // Really should use lilv_world_get_symbol()
            if (LilvNodePtr symbol{ lilv_world_get_symbol(gWorld, unit.get()) })
               units = LilvString(symbol.get());

         // Collect the value and range info
         bool hasLo = !std::isnan(minimumVals[i]);
         bool hasHi = !std::isnan(maximumVals[i]);
         float min = hasLo ? minimumVals[i] : 0.0f;
         float max = hasHi ? maximumVals[i] : 1.0f;
         float def = !std::isnan(defaultVals[i])
            ? defaultVals[i]
            : hasLo
               ? min
               : hasHi
                  ? max
                  : 0.0f;
   
         // Figure out the type of port we have
         bool toggle = isInput &&
            lilv_port_has_property(&plug, port, node_Toggled);
         bool enumeration = isInput &&
            lilv_port_has_property(&plug, port, node_Enumeration);
         bool integer = isInput &&
            lilv_port_has_property(&plug, port, node_Integer);
         bool sampleRate = isInput &&
            lilv_port_has_property(&plug, port, node_SampleRate);
         // Trigger properties can be combined with other types, but it
         // seems mostly to be combined with toggle.  So, we turn the
         // checkbox into a button.
         bool trigger = isInput &&
            lilv_port_has_property(&plug, port, node_Trigger);
         // We'll make the slider logarithmic
         bool logarithmic = isInput &&
            lilv_port_has_property(&plug, port, node_Logarithmic);

         // Get the scale points
         std::vector<double> scaleValues;
         wxArrayString scaleLabels;
         {
            using LilvScalePointsPtr =
               Lilv_ptr<LilvScalePoints, lilv_scale_points_free>;
            LilvScalePointsPtr points{
               lilv_port_get_scale_points(&plug, port) };
            LILV_FOREACH(scale_points, j, points.get()) {
               const auto point = lilv_scale_points_get(points.get(), j);
               scaleValues.push_back(
                  lilv_node_as_float(lilv_scale_point_get_value(point)));
               scaleLabels.push_back(
                  LilvString(lilv_scale_point_get_label(point)));
            }
         }

         const auto &controlPort = mControlPorts.emplace_back(
            std::make_shared<LV2ControlPort>(
               port, index, isInput, symbol, name, groupName,
               move(scaleValues), std::move(scaleLabels), units,
               min, max, def, hasLo, hasHi,
               toggle, enumeration, integer, sampleRate,
               trigger, logarithmic));
         // Figure out the type of port we have
         if (isInput)
            mControlPortMap[controlPort->mIndex] = mControlPorts.size() - 1;
         else if (controlPort->mIndex == latencyIndex)
            mLatencyPort = i;
      }
      // Check for atom ports
      else if (lilv_port_is_a(&plug, port, node_AtomPort)) {
         uint32_t minimumSize = 8192;
         if (LilvNodePtr min{ lilv_port_get(&plug, port, node_MinimumSize) }
            ; lilv_node_is_int(min.get())
         ){
            if (auto value = lilv_node_as_int(min.get())
               ; value > 0
            )
               minimumSize = std::max<uint32_t>(minimumSize, value);
         }
         bool wantsPosition =
            lilv_port_supports_event(&plug, port, node_Position);
         bool isMidi = lilv_port_supports_event(&plug, port, node_MidiEvent);
         if (isMidi)
            (isInput ? mMidiIn : mMidiOut) += 1;
         mAtomPorts.push_back(std::make_shared<LV2AtomPort>(
            port, index, isInput, symbol, name, groupName,
            minimumSize, isMidi, wantsPosition));
         bool isControl = lilv_node_equals(designation.get(), node_Control);
         if (isInput) {
            if (!mControlInIdx || isControl)
               mControlInIdx = mAtomPorts.size() - 1;
         }
         else if (!mControlOutIdx || isControl)
            mControlOutIdx = mAtomPorts.size() - 1;
      }
      // Check for CV ports
      else if (lilv_port_is_a(&plug, port, node_CVPort)) {
         // Collect the value and range info
         float min = 0;
         float max = 1;
         float def = 0;
         bool hasLo = false;
         bool hasHi = false;
         if (!std::isnan(minimumVals[i]))
            hasLo = true, min = minimumVals[i];
         if (!std::isnan(maximumVals[i]))
            hasHi = true, max = maximumVals[i];
         if (!std::isnan(defaultVals[i]))
            def = defaultVals[i];
         else if (hasLo)
            def = min;
         else if (hasHi)
            def = max;
         mCVPorts.push_back(std::make_shared<LV2CVPort>(
            port, index, isInput, symbol, name, groupName,
            min, max, def, hasLo, hasHi));
      }
   }
}

LV2PortStates::LV2PortStates(const LV2Ports &ports)
{
   for (auto &atomPort : ports.mAtomPorts)
      mAtomPortStates.emplace_back(
         std::make_shared<LV2AtomPortState>(atomPort));

   for (auto &cvPort : ports.mCVPorts)
      mCVPortStates.emplace_back(cvPort);
}

LV2PortUIStates::LV2PortUIStates(
   const LV2PortStates &portStates, const LV2Ports &ports)
{
   // Ignore control designation if one of them is missing
   if (ports.mControlInIdx && ports.mControlOutIdx) {
      mControlIn = portStates.mAtomPortStates[*ports.mControlInIdx];
      mControlOut = portStates.mAtomPortStates[*ports.mControlOutIdx];
   }

   for (auto &controlPort : ports.mControlPorts) {
      auto &state = mControlPortStates.emplace_back(controlPort);
      state.mLo = controlPort->mMin;
      state.mHi = controlPort->mMax;
      state.mLst = controlPort->mDef;
   }
}
