#include "render_job.h"

RenderScheduler::RenderScheduler() : worker_(&RenderScheduler::run, this) {}

RenderScheduler::~RenderScheduler() {
	shutdown();
}

RenderScheduler::Job RenderScheduler::submit(uint64_t generation, Work work) {
	auto job = std::make_shared<RenderJob>(generation);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) {
			job->fail();
			return job;
		}
		queue_.push_back({job, std::move(work)});
	}
	condition_.notify_one();
	return job;
}

bool RenderScheduler::cancel(const Job& job) {
	if (!job) return false;
	job->requestCancel();
	return true;
}

void RenderScheduler::shutdown() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_) return;
		stopping_ = true;
		for (auto& item : queue_)
			item.job->requestCancel();
	}
	condition_.notify_all();
	if (worker_.joinable()) worker_.join();
}

void RenderScheduler::run() {
	for (;;) {
		QueueItem item;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (queue_.empty()) {
				if (stopping_) return;
				continue;
			}
			item = std::move(queue_.front());
			queue_.pop_front();
		}

		if (item.job->cancelRequested()) {
			item.job->complete();
			continue;
		}

		item.job->start();
		try {
			if (item.work) item.work(item.job);
			item.job->complete();
		} catch (...) {
			item.job->fail();
		}
	}
}