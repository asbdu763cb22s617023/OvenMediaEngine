#include "segment_worker_manager.h"
#include "segment_stream_private.h"

#include <modules/http/server/http_connection.h>

//====================================================================================================
// SegmentWorker constructorc
//====================================================================================================
SegmentWorker::SegmentWorker()
{
	_stop_thread_flag = true;
}

//====================================================================================================
// SegmentWorker destructor
//====================================================================================================
SegmentWorker::~SegmentWorker()
{
	Stop();
}

//====================================================================================================
// SegmentWorker start
//====================================================================================================
bool SegmentWorker::Start(const SegmentProcessHandler &process_handler)
{
	if (!_stop_thread_flag)
	{
		return true;
	}

	_process_handler = process_handler;

	_stop_thread_flag = false;
	_worker_thread = std::thread(&SegmentWorker::WorkerThread, this);
	pthread_setname_np(_worker_thread.native_handle(), "SegWorker");

	return true;
}

//====================================================================================================
// SegmentWorker stop
//====================================================================================================
bool SegmentWorker::Stop()
{
	if (_stop_thread_flag)
	{
		return true;
	}

	_stop_thread_flag = true;
	// Generate Event
	_queue_event.Notify();
	if(_worker_thread.joinable())
	{
		_worker_thread.join();
	}

	return true;
}

//====================================================================================================
// add work info
//====================================================================================================
bool SegmentWorker::PushConnection(const std::shared_ptr<http::svr::HttpTransaction> &transaction)
{
	std::unique_lock<std::mutex> lock(_work_info_guard);
	_transaction_list_to_process.push(transaction);

	_queue_event.Notify();

	return true;
}

//====================================================================================================
// pop work info
//====================================================================================================
std::shared_ptr<http::svr::HttpTransaction> SegmentWorker::PopTransaction()
{
	std::unique_lock<std::mutex> lock(_work_info_guard);

	if (_transaction_list_to_process.empty())
		return nullptr;

	auto work_info = _transaction_list_to_process.front();
	_transaction_list_to_process.pop();

	return work_info;
}

//====================================================================================================
// WokrThread main loop
//====================================================================================================
void SegmentWorker::WorkerThread()
{
	while (!_stop_thread_flag)
	{
		// quequ event wait
		_queue_event.Wait();

		auto transaction = PopTransaction();

		if (transaction == nullptr)
		{
			if (_stop_thread_flag == false)
			{
				// It's a problem if there's nothing in the queue despite the event
				OV_ASSERT2(false);
			}

			continue;
		}

		if (_process_handler(transaction) == false)
		{
			logte("Segment process handler fail - target(%s)", transaction->GetRequest()->ToString());
		}

		//        logtd("Segment process handler - target(%s)", work_info->request_target.CStr());
	}
}

//====================================================================================================
// Start
//====================================================================================================
bool SegmentWorkerManager::Start(int worker_count, const SegmentProcessHandler &process_handler)
{
	if (worker_count <= 0)
		return false;

	// Create WorkerThread
	for (int index = 0; index < worker_count; index++)
	{
		auto worker = std::make_shared<SegmentWorker>();
		worker->Start(process_handler);
		_workers.push_back(worker);
	}

	return true;
}

//====================================================================================================
// Stop
//====================================================================================================
bool SegmentWorkerManager::Stop()
{
	for (const auto &worker : _workers)
	{
		worker->Stop();
	}

	return true;
}

//====================================================================================================
// Worker Add
//====================================================================================================
#define MAX_WORKER_INDEX 100000000
bool SegmentWorkerManager::PushConnection(const std::shared_ptr<http::svr::HttpTransaction> &transaction)
{
	int place = 0;
	if (transaction->GetConnection()->GetConnectionType() == http::ConnectionType::Http10 || 
		transaction->GetConnection()->GetConnectionType() == http::ConnectionType::Http11)
	{
		// HTTP/1.1 pipelining must be placed in the same thread as requests must be responded to in the order in which they were requested.
		place = transaction->GetConnection()->GetId() % _workers.size();
	}
	else if (transaction->GetConnection()->GetConnectionType() == http::ConnectionType::Http20)
	{
		// HTTP/2.0 can give a response to a transaction(stream) in any order.
		if (_worker_index < MAX_WORKER_INDEX)
		{
			_worker_index++;
		}
		else
		{
			_worker_index = 0;
		}
		
		place = transaction->GetConnection()->GetId() % _workers.size();
	}
	else
	{
		logte("Invalid connection type of transaction (%s) - target(%s)", transaction->GetRequest()->ToString(), 
		StringFromConnectionType(transaction->GetConnection()->GetConnectionType()));
		return false;
	}
	
	_workers[place]->PushConnection(transaction);

	return true;
}
