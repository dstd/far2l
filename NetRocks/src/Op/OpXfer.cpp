#include <utils.h>
#include <TimeUtils.h>
#include "OpXfer.h"
#include "../UI/Confirm.h"
#include "../UI/ConfirmOverwrite.h"
#include "../UI/WhatOnError.h"
#include "../lng.h"

OpXfer::OpXfer(int op_mode, std::shared_ptr<IHost> &base_host, const std::string &base_dir,
	std::shared_ptr<IHost> &dst_host, const std::string &dst_dir,
	struct PluginPanelItem *items, int items_count, XferKind kind, XferDirection direction)
	:
	OpBase(op_mode, base_host, base_dir,
		(direction == XK_UPLOAD) ? MNotificationUpload :
			((direction == XK_CROSSLOAD) ? MNotificationCrossload : MNotificationDownload)),

	_dst_host(dst_host),
	_dst_dir(dst_dir),
	_kind(kind),
	_direction(direction)
{
	_enumer = std::make_shared<Enumer>(_entries, _base_host, _base_dir, items, items_count, true, _state, _wea_state);
	_diffname_suffix = ".NetRocks@";
	_diffname_suffix+= TimeString(TSF_FOR_FILENAME);
}

bool OpXfer::Do()
{
	if (!IS_SILENT(_op_mode)) {
		if (!ConfirmXfer(_kind, _direction, _dst_dir).Ask(_default_xoa)) {
			fprintf(stderr, "NetRocks::Xfer: cancel\n");
			return false;
		}
	}

	if (_dst_dir.empty()) {
		_dst_dir = "./";
	} else if (_dst_dir[_dst_dir.size() - 1] != '.')
		_dst_dir+= '/';

	if (!StartThread()) {
		fprintf(stderr, "NetRocks::Xfer: start thread error\n");
		return false;
	}

	if (!WaitThread(IS_SILENT(_op_mode) ? 2000 : 500)) {
		XferProgress(_kind, _direction, _dst_dir, _state).Show();
		WaitThread();
	}

	return true;
}


void OpXfer::Process()
{
	if (_enumer) {
		_enumer->Scan();
		_enumer.reset();

		std::lock_guard<std::mutex> locker(_state.mtx);
		_state.stats.total_start = TimeMSNow();
		_state.stats.total_paused = std::chrono::milliseconds::zero();
	}

	Transfer();
}


void OpXfer::Transfer()
{
	std::string path_local;
	for (const auto &e : _entries) {
		const std::string &subpath = e.first.substr(_base_dir.size());
		path_local = _dst_dir;
		path_local+= subpath;

		{
			std::unique_lock<std::mutex> lock(_state.mtx);
			_state.path = subpath;
			_state.stats.file_complete = 0;
			_state.stats.file_total = S_ISDIR(e.second.mode) ? 0 : e.second.size;
			_state.stats.current_start = TimeMSNow();
			_state.stats.current_paused = std::chrono::milliseconds::zero();
		}

		if (S_ISDIR(e.second.mode)) {
			WhatOnErrorWrap<WEK_MAKEDIR>(_wea_state, _state, _base_host.get(), path_local,
				[&] () mutable 
				{
					_dst_host->DirectoryCreate(path_local, e.second.mode);
				}
			);

		} else {
			unsigned long long pos = 0;

			FileInformation existing_file_info;
			bool existing = false;
			try {
				_dst_host->GetInformation(existing_file_info, path_local);
				existing = true;
			} catch (std::exception &ex) { // FIXME: distinguish unexistence of file from IO failure
				;
			}

			if (existing) {
				auto xoa = _default_xoa;
				if (xoa == XOA_ASK) {
					xoa = ConfirmOverwrite(_kind, _direction, path_local, e.second.modification_time, e.second.size,
								existing_file_info.modification_time, existing_file_info.size).Ask(_default_xoa);
					if (xoa == XOA_CANCEL) {
						return;
					}
				}
				if (xoa == XOA_OVERWRITE_IF_NEWER) {
					if (e.second.modification_time.tv_sec < existing_file_info.modification_time.tv_sec ||
					(e.second.modification_time.tv_sec == existing_file_info.modification_time.tv_sec
						&& e.second.modification_time.tv_nsec <= existing_file_info.modification_time.tv_nsec)) {
						xoa = XOA_SKIP;
					} else {
						xoa = XOA_OVERWRITE;
					}
				}
				if (xoa == XOA_RESUME) {
					if (existing_file_info.size < e.second.size) {
						_state.stats.all_complete+= existing_file_info.size;
						_state.stats.file_complete+= existing_file_info.size;
						pos = existing_file_info.size;
					} else {
						xoa = XOA_SKIP;
					}

				} else if (xoa == XOA_CREATE_DIFFERENT_NAME) {
					path_local+= _diffname_suffix;
				}

				if (xoa == XOA_SKIP) {
					std::unique_lock<std::mutex> lock(_state.mtx);
					_state.stats.all_complete+= e.second.size;
					_state.stats.file_complete+= e.second.size;
					_state.stats.count_complete++;
					continue;
				}
			}
			try {
				FileCopyLoop(e.first, path_local, pos, e.second.mode);
				if (_kind == XK_MOVE) {
					WhatOnErrorWrap<WEK_RMFILE>(_wea_state, _state, _base_host.get(), e.first,
						[&] () mutable 
						{
							_base_host->FileDelete(e.first);
						}
					);
				}
			} catch (std::exception &ex) {
				fprintf(stderr, "NetRocks::Xfer: %s on '%s' -> '%s'\n", ex.what(), e.first.c_str(), path_local.c_str());
			}
		}

		ProgressStateUpdate psu(_state);
		_state.stats.count_complete++;
	}

	if (_kind == XK_MOVE) {
		for (auto rev_i = _entries.rbegin(); rev_i != _entries.rend(); ++rev_i) {
			if (S_ISDIR(rev_i->second.mode)) {
				WhatOnErrorWrap<WEK_RMDIR>(_wea_state, _state, _base_host.get(), rev_i->first,
					[&] () mutable 
					{
						_base_host->DirectoryDelete(rev_i->first);
					}
				);
			}
		}
	}
}


void OpXfer::FileCopyLoop(const std::string &path_src, const std::string &path_dst, unsigned long long pos, mode_t mode)
{
	IHost *indicted = nullptr;
	for (unsigned long long prev_attempt_pos = pos;;) try {
		if (indicted) { // retrying...
			indicted->ReInitialize();

			indicted = _dst_host.get();
			pos = _dst_host->GetSize(path_dst);
		}

		indicted = _base_host.get();
		std::shared_ptr<IFileReader> reader = _base_host->FileGet(path_src, pos);
		indicted = _dst_host.get();
		std::shared_ptr<IFileWriter> writer = _dst_host->FilePut(path_dst, mode, pos);
		char buf[0x10000];
		for (;;) {
			indicted = _base_host.get();
			const size_t piece = reader->Read(buf, sizeof(buf));
			if (piece == 0) {
				break;
			}
			indicted = _dst_host.get();
			writer->Write(buf, piece);

			indicted = nullptr;
			ProgressStateUpdate psu(_state);
			_state.stats.all_complete+= piece;
			_state.stats.file_complete+= piece;
		}
		break;

	} catch (AbortError &) {
		throw;

	} catch (std::exception &ex) {
		if (indicted == nullptr) {
			throw;
		}

		switch (_wea_state.Query( (_direction == XK_UPLOAD) ? WEK_UPLOAD
				: ((_direction == XK_DOWNLOAD) ? WEK_DOWNLOAD : WEK_CROSSLOAD) ,
				ex.what(), path_src, indicted->SiteName())) {

			case WEA_SKIP: {
				ProgressStateUpdate psu(_state);
				_state.stats.count_skips++;
			} return;

			case WEA_RETRY: {
				if (prev_attempt_pos == pos) {
					sleep(1);
				} else {
					prev_attempt_pos = pos;
				}

				ProgressStateUpdate psu(_state);
				_state.stats.count_retries++;
			} break;

			default:
				throw;
		}
	}
}

void OpXfer::ForcefullyAbort()
{
	_dst_host->Abort();
	OpBase::ForcefullyAbort();
}

