/**********************************************************************

Audacity: A Digital Audio Editor

ProjectAudioIO.cpp

Paul Licameli split from AudacityProject.cpp

**********************************************************************/

#include "ProjectAudioIO.h"

#include "AudioIO.h"
#include "Project.h"

static const AudacityProject::AttachedObjects::RegisteredFactory sAudioIOKey{
  []( AudacityProject &parent ){
     return std::make_shared< ProjectAudioIO >( parent );
   }
};

ProjectAudioIO &ProjectAudioIO::Get( AudacityProject &project )
{
   return project.AttachedObjects::Get< ProjectAudioIO >( sAudioIOKey );
}

const ProjectAudioIO &ProjectAudioIO::Get( const AudacityProject &project )
{
   return Get( const_cast<AudacityProject &>(project) );
}

ProjectAudioIO::ProjectAudioIO( AudacityProject &project )
: mProject{ project }
{
}

ProjectAudioIO::~ProjectAudioIO()
{
}

int ProjectAudioIO::GetAudioIOToken() const
{
   return mAudioIOToken;
}

void ProjectAudioIO::SetAudioIOToken(int token)
{
   mAudioIOToken = token;
}

bool ProjectAudioIO::IsAudioActive() const
{
   return GetAudioIOToken() > 0 &&
      gAudioIO->IsStreamActive(GetAudioIOToken());
}

MeterPanel *ProjectAudioIO::GetPlaybackMeter()
{
   return mPlaybackMeter;
}

void ProjectAudioIO::SetPlaybackMeter(MeterPanel *playback)
{
   auto &project = mProject;
   mPlaybackMeter = playback;
   if (gAudioIO)
   {
      gAudioIO->SetPlaybackMeter( &project , mPlaybackMeter );
   }
}

MeterPanel *ProjectAudioIO::GetCaptureMeter()
{
   return mCaptureMeter;
}

void ProjectAudioIO::SetCaptureMeter(MeterPanel *capture)
{
   auto &project = mProject;
   mCaptureMeter = capture;

   if (gAudioIO)
   {
      gAudioIO->SetCaptureMeter( &project, mCaptureMeter );
   }
}
