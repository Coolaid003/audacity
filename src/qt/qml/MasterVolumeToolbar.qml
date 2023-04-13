import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Audacity
import Audacity.UiComponents

Item {
   id: root
   height: parent.height
   implicitHeight: height
   implicitWidth: contents.width

   objectName: "MasterVolumeToolbar"

   property bool testerVisible: false
   property bool gripVisible: false
   property bool separatorVisible: false
   property int nextChannel: 0

   signal updateStatusBar(status: string)

   RowLayout {
      id: contents
      anchors.verticalCenter: parent.verticalCenter
      spacing: 8

      ToolbarGrip {
         id: grip
         visible: root.gripVisible
      }

      RowLayout {
         spacing: 2

         Rectangle {
            height: root.height
            width: root.gripVisible ? 2 : 6
            color: appConfig.backgroundColor1
         }

         VolumeControl {
            id: volumeControl
         }

         VolumeControlTester {
            id: volumeControlTester
            visible: testerVisible
            height: root.height
            channels: 2

            onReset: volumeControl.reset()

            onSendData: function(channel, data) {
               if (channel === 0) {
                  volumeControl.setLeftChannelValue(data)
               } else {
                  volumeControl.setRightChannelValue(data)
               }
            }

            onSendRandomData: function(data) {
               if (nextChannel === 0) {
                  volumeControl.setLeftChannelValue(data)
                  nextChannel = 1
               } else {
                  volumeControl.setRightChannelValue(data)
                  nextChannel = 0
               }
            }
         }
      }

      ToolbarSeparator {
         visible: separatorVisible
      }
   }
}