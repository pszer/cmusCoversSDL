// TODO - If I give up the ability to have a cover file be the music filename just with image extension, I can store the last song as the last folder and not rescan the same folder again. Useful for those who play songs in order.
// TODO - Allow wildcard so if we have things like "cover1.jpg" we can match it
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "config.h"
#include <time.h>

volatile sig_atomic_t forceRefresh=0;

// Catch SIGUSR1
static void refreshcatch(const int signo){
	(void)signo;
	forceRefresh=1;
}
char* maybeGetCoverFilename(const char* _passedCandidate, const char* _passedAcceptable, const char* _passedFolderPrefix, char _filenameCaseSensitive){
	char _res;
	if (_filenameCaseSensitive){
		_res=(strncmp(_passedCandidate,_passedAcceptable,strlen(_passedAcceptable))==0);
	}else{
		_res=(strncasecmp(_passedCandidate,_passedAcceptable,strlen(_passedAcceptable))==0);
	}
	if (_res){
		int j;
		for (j=0;j<NUMEXTENSIONS;++j){
			if (strncasecmp(&(_passedCandidate[strlen(_passedAcceptable)]),loadableExtensions[j],strlen(loadableExtensions[j]))==0){
				char* _gottenCoverFilename = malloc(strlen(_passedFolderPrefix)+strlen(_passedCandidate)+1);
				strcpy(_gottenCoverFilename,_passedFolderPrefix);
				strcat(_gottenCoverFilename,_passedCandidate);
				return _gottenCoverFilename;
			}
		}
	}
	return NULL;
}
FILE* goodpopen(char* const _args[]){
	int _crossFiles[2]; // File descriptors that work for both processes
	if (pipe(_crossFiles)!=0){
		return NULL;
	}
	pid_t _newProcess = fork();
	if (_newProcess==-1){
		close(_crossFiles[0]);
		return NULL;
	}else if (_newProcess==0){ // 0 is returned to the new process
		close(_crossFiles[0]); // Child does not ned to read
		dup2(_crossFiles[1], STDOUT_FILENO); // make _crossFiles[1] be the same as STDOUT_FILENO
		// First arg is the path of the file again
		execv(_args[0],_args); // This will do this program and then end the child process
		exit(1); // This means execv failed
	}

	close(_crossFiles[1]); // Parent doesn't need to write
	FILE* _ret = fdopen(_crossFiles[0],"r");
	waitpid(_newProcess,NULL,0);
	return _ret;
}
void seekPast(FILE* fp, unsigned char _target){
	while (1){
		int _lastRead=fgetc(fp);
		if (_lastRead==_target || _lastRead==EOF){
			break;
		}
	}
}
void seekNextLine(FILE* fp){
	seekPast(fp,0x0A);
}
int main(int argc, char const *argv[]){
	// Catch SIGUSR1. Use it as a signal to refresh
	struct sigaction refreshsig;
	memset(&refreshsig, 0, sizeof(refreshsig));
	refreshsig.sa_handler = refreshcatch;
	sigaction(SIGUSR1, &refreshsig, NULL);

	struct timespec _eventCheckTime;
	_eventCheckTime.tv_sec=0;
	_eventCheckTime.tv_nsec = EVENTPOLLTIME*1000000L;

	SDL_Window* mainWindow;
	SDL_Renderer* mainWindowRenderer;
	SDL_Init(SDL_INIT_VIDEO);
	IMG_Init( IMG_INIT_PNG );
	IMG_Init( IMG_INIT_JPG );
	mainWindow = SDL_CreateWindow(WINDOWTITLE,SDL_WINDOWPOS_UNDEFINED,SDL_WINDOWPOS_UNDEFINED,640,480,SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN );
	if (!mainWindow){
		printf("SDL_CreateWindow failed\n");
		return 1;
	}
	mainWindowRenderer = SDL_CreateRenderer( mainWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!mainWindowRenderer){
		printf("SDL_CreateRenderer failed.\n");
		return 1;
	}
	unsigned int lastRefresh=0;
	char* _currentFilename=NULL;
	SDL_Texture* _currentImage=NULL;
	char running=1;
	while(running){
		SDL_Event e;
		while(SDL_PollEvent(&e)!=0){
			if(e.type==SDL_QUIT){
				running=0;
			}else if(e.type==SDL_KEYDOWN){
				if (e.key.keysym.sym==SDLK_q){
					running=0;
				}
			}
		}
		unsigned int _curTime=SDL_GetTicks()+RECHECKMILLI; // Offset all time values by RECHECKMILLI so when the program starts it'll check if RECHECKMILLI > 0 
		if (_curTime>=lastRefresh+RECHECKMILLI){
			lastRefresh=_curTime;
			// Use cmus-remote to see if we've got a new song playing
			FILE* _cmusRes = goodpopen(_programArgs);
			char _foundFile=0;
			while (!feof(_cmusRes)){
				// Find the 'file' line
				if (fgetc(_cmusRes)!='f'){
					seekNextLine(_cmusRes);
				}else{
					_foundFile=1;
					seekPast(_cmusRes,' '); // Seek to the end of 'file '
					char* _readFile=NULL;
					size_t _readLen=0;
					if (getline(&_readFile,&_readLen,_cmusRes)!=-1){
						// Strip newline left on the string by getline
						int _cachedStrlen=strlen(_readFile);
						if (_readFile[_cachedStrlen-1]=='\n'){
							_readFile[--_cachedStrlen]='\0';
						}
						// If the filename of the current song is different from the last one
						if (_currentFilename==NULL || strcmp(_readFile,_currentFilename)!=0){
							printf("Looking for art\n");
							if (_currentImage){ // No matter what, we don't want the old art anymore
								SDL_DestroyTexture(_currentImage);
								_currentImage=NULL;
							}
							free(_currentFilename);
							_currentFilename=_readFile;
							// Find the folder of the song file by finding the last slash
							char* _pathSongFolder=strdup(_currentFilename); // Once it contains a folder path, that folder path will end with DIRSEPARATORCHAR
							int _currentSubDirUp=0;
							for (_currentSubDirUp=0;_currentSubDirUp<=MAXDIRUP;++_currentSubDirUp){
								int i;
								for (i=strlen(_pathSongFolder)-2;i>=0;--i){ // Skip the ending char. This goes over either one char of the filename or the end DIRSEPARATORCHAR from the last path
									if (_pathSongFolder[i]==DIRSEPARATORCHAR){
										_pathSongFolder[i+1]='\0';
										break;
									}
								}
								if (i>=0){ // If we actually found a new _pathSongFolder
									// Find cover image in folder
									DIR* _songFolder=opendir(_pathSongFolder);
									if (_songFolder!=NULL){
										#if SAMEASSONGNAMECOVER
											int _musicNoExtensionLen=-1;
											for (i=_cachedStrlen-1;i>0;--i){
												if (_currentFilename[i]=='.'){
													_musicNoExtensionLen=i+1;
													break;
												}
											}
											int _cachedDirLength = strlen(_pathSongFolder);
											// Temporarily trim the extension off the filename
											char _oldChar = _currentFilename[_musicNoExtensionLen];
											_currentFilename[_musicNoExtensionLen]='\0';
										#endif
										errno=0;
										char* _gottenCoverFilename=NULL;
										struct dirent* _currentEntry;
										while(_gottenCoverFilename==NULL && (_currentEntry=readdir(_songFolder))){
											#if SAMEASSONGNAMECOVER
												_gottenCoverFilename = maybeGetCoverFilename(_currentEntry->d_name,&(_currentFilename[_cachedDirLength]),_pathSongFolder,1);
											#endif
											if (!_gottenCoverFilename){
												// Find out if this is a possible cover
												for (i=0;i<NUMCOVERFILENAMES;++i){
													//char* maybeGetCoverFilename(const char* _passedCandidate, const char* _passedAcceptable, const char* _passedFolderPrefix){
													if (_gottenCoverFilename = maybeGetCoverFilename(_currentEntry->d_name,coverFilenames[i],_pathSongFolder,0)){
														break;
													}
												}
											}
										}
										#if SAMEASSONGNAMECOVER
											// Restore the filename to be complete again
											_currentFilename[_musicNoExtensionLen]=_oldChar;
										#endif
										// Load the cover image if found
										if (_gottenCoverFilename!=NULL){
											printf("Got art %s\n",_gottenCoverFilename!=NULL ? _gottenCoverFilename : "NULL");	
											SDL_Surface* _tempSurface=IMG_Load(_gottenCoverFilename);
											if (_tempSurface!=NULL){
												if ((_currentImage = SDL_CreateTextureFromSurface(mainWindowRenderer,_tempSurface))==NULL){
													printf("SDL_CreateTextureFromSurface failed\n");
												}
												SDL_FreeSurface(_tempSurface);
											}else{
												printf("Failed to load %s with error %s\n",_gottenCoverFilename,IMG_GetError());
											}
										}
										free(_gottenCoverFilename);
										if (errno!=0){
											printf("Failure when reading directory entries, error %d\n",errno);
										}
										if (closedir(_songFolder)==-1){
											printf("Failed to close dir\n");
										}
									}else{
										printf("Failed to open folder %s\n",_pathSongFolder);
									}
								}else{
									printf("Failed to get folder from path\n");
								}
							}
							free(_pathSongFolder);
						}
					}
					if (_currentFilename!=_readFile){
						free(_readFile);
					}
				}
			}
			if (!_foundFile){ // If cmus isn't running you wouldn't find a file
				if (_currentImage){
					SDL_DestroyTexture(_currentImage);
					_currentImage=NULL;
				}
				free(_currentFilename);
				_currentFilename=NULL;
			}
			fclose(_cmusRes);
		}
		// SDL says you must redraw everything every time you use SDL_RenderPresent
		SDL_SetRenderDrawColor(mainWindowRenderer,0,0,0,255);
		SDL_RenderClear(mainWindowRenderer);
		if (_currentImage!=NULL){
			SDL_Rect _srcRect;
			_srcRect.x=0;
			_srcRect.y=0;
			SDL_QueryTexture(_currentImage, NULL, NULL, &(_srcRect.w), &(_srcRect.h));

			SDL_Rect _destRect;
			int _windowWidth;
			int _windowHeight;
			SDL_GetWindowSize(mainWindow,&_windowWidth,&_windowHeight);
			_windowWidth-=PIXELPADDING*2;
			_windowHeight-=PIXELPADDING*2;

			int _imgW;
			int _imgH;
			SDL_QueryTexture(_currentImage,NULL,NULL,&_imgW,&_imgH);

			double _scaleFactor;
			if (_windowWidth/(double)_imgW<_windowHeight/(double)_imgH){
				_scaleFactor=_windowWidth/(double)_imgW;
			}else{
				_scaleFactor=_windowHeight/(double)_imgH;
			}
			_destRect.w=_srcRect.w*_scaleFactor;
			_destRect.h=_srcRect.h*_scaleFactor;
			_destRect.x=PIXELPADDING+(_windowWidth-_destRect.w)/2;
			_destRect.y=PIXELPADDING+(_windowHeight-_destRect.h)/2;

			SDL_RenderCopy(mainWindowRenderer, _currentImage, &_srcRect, &_destRect );
		}
		SDL_RenderPresent(mainWindowRenderer);
		int _nextCmusCheckTime = (lastRefresh+RECHECKMILLI+10)-_curTime;
		#if WAITMODE == 0
			// Based on SDL_WaitEventTimeout
			unsigned int _maxTime = SDL_GetTicks()+_nextCmusCheckTime;
			while(1){
				SDL_PumpEvents();
				int _numNewEvents = SDL_PeepEvents(NULL,1,SDL_GETEVENT,SDL_FIRSTEVENT,SDL_LASTEVENT);
				if (_numNewEvents!=0){ // Accounts for errors too
					break;
				}else{
					if (forceRefresh || SDL_GetTicks()>=_maxTime){
						forceRefresh=0;
						lastRefresh=0;
						break;
					}else{
						nanosleep(&_eventCheckTime,NULL);
					}
				}
			}
		#elif WAITMODE == 1
			struct timespec _waitTime;
			_waitTime.tv_sec=0;
			_waitTime.tv_nsec = ALTWAITMODEREDRAWTIME < _nextCmusCheckTime*1000000L ? ALTWAITMODEREDRAWTIME : _nextCmusCheckTime*1000000L;
			nanosleep(&_waitTime,NULL);
		#endif
	}
	free(_currentFilename);
	SDL_DestroyWindow(mainWindow);
	return 0;
}