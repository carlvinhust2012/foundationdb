/*
 * BlobGranuleVerifier.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <map>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "fdbclient/BlobGranuleReader.actor.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/BlobGranuleValidation.actor.h"
#include "fdbserver/BlobGranuleServerCommon.actor.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/genericactors.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

#define BGV_DEBUG true

/*
 * This workload is designed to verify the correctness of the blob data produced by the blob workers.
 * As a read-only validation workload, it can piggyback off of other write or read/write workloads.
 * To verify the data outside FDB's 5 second MVCC window, it tests time travel reads by doing an initial comparison at
 * the latest read version, and then waiting a period of time to re-read the data from blob.
 * To catch availability issues with the blob worker, it does a request to each granule at the end of the test.
 */
struct BlobGranuleVerifierWorkload : TestWorkload {
	bool doSetup;
	double testDuration;
	double timeTravelLimit;
	uint64_t timeTravelBufferSize;
	int threads;
	int64_t errors = 0;
	int64_t mismatches = 0;
	int64_t initialReads = 0;
	int64_t timeTravelReads = 0;
	int64_t timeTravelTooOld = 0;
	int64_t rowsRead = 0;
	int64_t bytesRead = 0;
	int64_t purges = 0;
	std::vector<Future<Void>> clients;
	bool enablePurging;
	bool initAtEnd;
	bool strictPurgeChecking;
	bool doForcePurge;
	bool clearAndMergeCheck;

	DatabaseConfiguration config;

	Reference<BlobConnectionProvider> bstore;
	AsyncVar<Standalone<VectorRef<KeyRangeRef>>> granuleRanges;

	bool startedForcePurge;
	Optional<Key> forcePurgeKey;
	Version forcePurgeVersion;

	std::vector<std::tuple<KeyRange, Version, UID, Future<GranuleFiles>>> purgedDataToCheck;

	BlobGranuleVerifierWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		doSetup = !clientId; // only do this on the "first" client
		testDuration = getOption(options, LiteralStringRef("testDuration"), 120.0);
		timeTravelLimit = getOption(options, LiteralStringRef("timeTravelLimit"), testDuration);
		timeTravelBufferSize = getOption(options, LiteralStringRef("timeTravelBufferSize"), 100000000);
		threads = getOption(options, LiteralStringRef("threads"), 1);
		enablePurging = getOption(options, LiteralStringRef("enablePurging"), sharedRandomNumber % 3 == 0);
		sharedRandomNumber /= 3;
		// FIXME: re-enable this! There exist several bugs with purging active granules where a small amount of state
		// won't be cleaned up.
		strictPurgeChecking =
		    getOption(options, LiteralStringRef("strictPurgeChecking"), false /*sharedRandomNumber % 2 == 0*/);
		sharedRandomNumber /= 2;

		doForcePurge = getOption(options, LiteralStringRef("doForcePurge"), sharedRandomNumber % 3 == 0);
		sharedRandomNumber /= 3;

		// randomly some tests write data first and then turn on blob granules later, to test conversion of existing DB
		initAtEnd = !enablePurging && sharedRandomNumber % 10 == 0;
		sharedRandomNumber /= 10;

		clearAndMergeCheck = getOption(options, LiteralStringRef("clearAndMergeCheck"), sharedRandomNumber % 10 == 0);
		sharedRandomNumber /= 10;

		startedForcePurge = false;

		if (doSetup && BGV_DEBUG) {
			fmt::print("BlobGranuleVerifier starting\n");
			fmt::print("  enablePurging={0}\n", enablePurging);
			fmt::print("  strictPurgeChecking={0}\n", strictPurgeChecking);
			fmt::print("  doForcePurge={0}\n", doForcePurge);
		}

		ASSERT(threads >= 1);
		try {
			bstore = BlobConnectionProvider::newBlobConnectionProvider(SERVER_KNOBS->BG_URL);
		} catch (Error& e) {
			if (BGV_DEBUG) {
				printf("Blob Granule Verifier got backup container init error %s\n", e.name());
			}
			throw e;
		}
	}

	// FIXME: run the actual FDBCLI command instead of copy/pasting its implementation
	// Sets the whole user keyspace to be blobified
	ACTOR Future<Void> setUpBlobRange(Database cx) {
		state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
		loop {
			try {
				tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
				tr->set(blobRangeChangeKey, deterministicRandom()->randomUniqueID().toString());
				wait(krmSetRange(tr, blobRangeKeys.begin, KeyRange(normalKeys), LiteralStringRef("1")));
				wait(tr->commit());
				if (BGV_DEBUG) {
					printf("Successfully set up blob granule range for normalKeys\n");
				}
				TraceEvent("BlobGranuleVerifierSetup");
				return Void();
			} catch (Error& e) {
				wait(tr->onError(e));
			}
		}
	}

	std::string description() const override { return "BlobGranuleVerifier"; }
	Future<Void> setup(Database const& cx) override { return _setup(cx, this); }

	ACTOR Future<Void> _setup(Database cx, BlobGranuleVerifierWorkload* self) {
		if (!self->doSetup) {
			wait(delay(0));
			return Void();
		}

		wait(success(ManagementAPI::changeConfig(cx.getReference(), "blob_granules_enabled=1", true)));

		if (!self->initAtEnd) {
			wait(self->setUpBlobRange(cx));
		}
		return Void();
	}

	ACTOR Future<Void> findGranules(Database cx, BlobGranuleVerifierWorkload* self) {
		loop {
			state Transaction tr(cx);
			loop {
				try {
					Standalone<VectorRef<KeyRangeRef>> allGranules = wait(tr.getBlobGranuleRanges(normalKeys));
					self->granuleRanges.set(allGranules);
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
			wait(delay(deterministicRandom()->random01() * 10.0));
		}
	}

	struct OldRead {
		KeyRange range;
		Version v;
		RangeResult oldResult;

		OldRead() {}
		OldRead(KeyRange range, Version v, RangeResult oldResult) : range(range), v(v), oldResult(oldResult) {}
	};

	ACTOR Future<Void> killBlobWorkers(Database cx, BlobGranuleVerifierWorkload* self) {
		state Transaction tr(cx);
		state std::set<UID> knownWorkers;
		state bool first = true;
		loop {
			try {
				RangeResult r = wait(tr.getRange(blobWorkerListKeys, CLIENT_KNOBS->TOO_MANY));

				state std::vector<UID> haltIds;
				state std::vector<Future<ErrorOr<Void>>> haltRequests;
				for (auto& it : r) {
					BlobWorkerInterface interf = decodeBlobWorkerListValue(it.value);
					if (first) {
						knownWorkers.insert(interf.id());
					}
					if (knownWorkers.count(interf.id())) {
						haltIds.push_back(interf.id());
						haltRequests.push_back(interf.haltBlobWorker.tryGetReply(HaltBlobWorkerRequest(1e6, UID())));
					}
				}
				first = false;
				wait(waitForAll(haltRequests));
				bool allPresent = true;
				for (int i = 0; i < haltRequests.size(); i++) {
					if (haltRequests[i].get().present()) {
						knownWorkers.erase(haltIds[i]);
					} else {
						allPresent = false;
					}
				}
				if (allPresent) {
					return Void();
				} else {
					wait(delay(1.0));
				}
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	// TODO refactor more generally
	ACTOR Future<Void> loadGranuleMetadataBeforeForcePurge(Database cx, BlobGranuleVerifierWorkload* self) {
		// load all granule history entries that intersect purged range
		state KeyRange cur = blobGranuleHistoryKeys;
		state Transaction tr(cx);
		loop {
			try {
				RangeResult history = wait(tr.getRange(cur, 100));
				for (auto& it : history) {
					KeyRange keyRange;
					Version version;
					std::tie(keyRange, version) = decodeBlobGranuleHistoryKey(it.key);
					// TODO: filter by key range for partial key range purge
					Standalone<BlobGranuleHistoryValue> historyValue = decodeBlobGranuleHistoryValue(it.value);

					Future<GranuleFiles> fileFuture = loadHistoryFiles(cx, historyValue.granuleID);
					self->purgedDataToCheck.push_back({ keyRange, version, historyValue.granuleID, fileFuture });
				}
				if (!history.empty() && history.more) {
					cur = KeyRangeRef(keyAfter(history.back().key), cur.end);
				} else {
					break;
				}
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		// wait for file loads to finish
		state int i;
		for (i = 0; i < self->purgedDataToCheck.size(); i++) {
			wait(success(std::get<3>(self->purgedDataToCheck[i])));
		}

		if (BGV_DEBUG) {
			fmt::print("BGV loaded {0} granules metadata before force purge\n", self->purgedDataToCheck.size());
		}

		ASSERT(!self->purgedDataToCheck.empty());

		return Void();
	}

	ACTOR Future<Void> verifyGranules(Database cx, BlobGranuleVerifierWorkload* self, bool allowPurging) {
		state double last = now();
		state double endTime = last + self->testDuration;
		state std::map<double, OldRead> timeTravelChecks;
		state int64_t timeTravelChecksMemory = 0;
		state Version prevPurgeVersion = -1;
		state UID dbgId = debugRandom()->randomUniqueID();

		TraceEvent("BlobGranuleVerifierStart");
		if (BGV_DEBUG) {
			printf("BGV thread starting\n");
		}

		// wait for first set of ranges to be loaded
		wait(self->granuleRanges.onChange());

		if (BGV_DEBUG) {
			printf("BGV got ranges\n");
		}

		loop {
			try {
				state double currentTime = now();
				state std::map<double, OldRead>::iterator timeTravelIt = timeTravelChecks.begin();
				while (timeTravelIt != timeTravelChecks.end() && currentTime >= timeTravelIt->first) {
					state OldRead oldRead = timeTravelIt->second;
					timeTravelChecksMemory -= oldRead.oldResult.expectedSize();
					// advance iterator before doing read, so if it gets error we don't retry it
					timeTravelIt = timeTravelChecks.erase(timeTravelIt);
					if (prevPurgeVersion == -1) {
						prevPurgeVersion = oldRead.v;
					}

					// before doing read, purge just before read version
					state Version newPurgeVersion = 0;
					state bool doPurging = allowPurging && deterministicRandom()->random01() < 0.5;
					state bool forcePurge = doPurging && self->doForcePurge && deterministicRandom()->random01() < 0.25;
					if (doPurging) {
						CODE_PROBE(true, "BGV considering purge");
						Version maxPurgeVersion = oldRead.v;
						for (auto& it : timeTravelChecks) {
							maxPurgeVersion = std::min(it.second.v, maxPurgeVersion);
						}
						if (prevPurgeVersion < maxPurgeVersion) {
							CODE_PROBE(true, "BGV doing purge");
							newPurgeVersion = deterministicRandom()->randomInt64(prevPurgeVersion, maxPurgeVersion);
							prevPurgeVersion = std::max(prevPurgeVersion, newPurgeVersion);
							if (BGV_DEBUG) {
								fmt::print("BGV Purging @ {0}{1}\n", newPurgeVersion, forcePurge ? " (force)" : "");
							}
							try {
								if (forcePurge) {
									wait(self->loadGranuleMetadataBeforeForcePurge(cx, self));
									self->startedForcePurge = true;
								}
								Key purgeKey = wait(cx->purgeBlobGranules(normalKeys, newPurgeVersion, {}, forcePurge));
								if (forcePurge) {
									self->forcePurgeKey = purgeKey;
									self->forcePurgeVersion = newPurgeVersion;
									if (BGV_DEBUG) {
										fmt::print("BGV Force purge registered, stopping\n");
									}
									return Void();
								}
								if (BGV_DEBUG) {
									fmt::print("BGV Purged @ {0}, waiting\n", newPurgeVersion);
								}
								wait(cx->waitPurgeGranulesComplete(purgeKey));
							} catch (Error& e) {
								if (e.code() == error_code_operation_cancelled) {
									throw e;
								}
								// purging shouldn't error, it should retry.
								if (BGV_DEBUG) {
									fmt::print("Unexpected error {0} purging @ {1}!\n", e.name(), newPurgeVersion);
								}
								ASSERT(false);
							}
							CODE_PROBE(true, "BGV purge complete");
							if (BGV_DEBUG) {
								fmt::print("BGV Purge complete @ {0}\n", newPurgeVersion);
							}
							self->purges++;
						} else {
							doPurging = false;
						}
					}

					// do time travel read
					try {
						std::pair<RangeResult, Standalone<VectorRef<BlobGranuleChunkRef>>> reReadResult =
						    wait(readFromBlob(cx, self->bstore, oldRead.range, 0, oldRead.v));
						if (!compareFDBAndBlob(oldRead.oldResult, reReadResult, oldRead.range, oldRead.v, BGV_DEBUG)) {
							self->mismatches++;
						}
						self->timeTravelReads++;
					} catch (Error& e) {
						fmt::print("Error TT: {0}\n", e.name());
						if (e.code() == error_code_blob_granule_transaction_too_old) {
							self->timeTravelTooOld++;
							// TODO: add debugging info for when this is a failure
							fmt::print("BGV ERROR: TTO [{0} - {1}) @ {2}\n",
							           oldRead.range.begin.printable(),
							           oldRead.range.end.printable(),
							           oldRead.v);
						}
					}

					// if purged just before read, verify that purge cleaned up data by restarting blob workers and
					// reading older than the purge version
					if (doPurging) {
						if (self->strictPurgeChecking) {
							wait(self->killBlobWorkers(cx, self));
							if (BGV_DEBUG) {
								fmt::print("BGV Reading post-purge [{0} - {1}) @ {2}\n",
								           oldRead.range.begin.printable(),
								           oldRead.range.end.printable(),
								           prevPurgeVersion);
							}
						}
						// ensure purge version exactly is still readable
						std::pair<RangeResult, Standalone<VectorRef<BlobGranuleChunkRef>>> versionRead1 =
						    wait(readFromBlob(cx, self->bstore, oldRead.range, 0, prevPurgeVersion));
						if (BGV_DEBUG) {
							fmt::print("BGV Post-purge first read:\n");
							printGranuleChunks(versionRead1.second);
						}
						if (self->strictPurgeChecking) {
							try {
								// read at purgeVersion - 1, should NOT be readable
								Version minSnapshotVersion = newPurgeVersion;
								for (auto& it : versionRead1.second) {
									minSnapshotVersion = std::min(minSnapshotVersion, it.snapshotVersion);
								}
								if (BGV_DEBUG) {
									fmt::print("BGV Reading post-purge again [{0} - {1}) @ {2}\n",
									           oldRead.range.begin.printable(),
									           oldRead.range.end.printable(),
									           minSnapshotVersion - 1);
								}
								std::pair<RangeResult, Standalone<VectorRef<BlobGranuleChunkRef>>> versionRead2 =
								    wait(readFromBlob(cx, self->bstore, oldRead.range, 0, minSnapshotVersion - 1));
								if (BGV_DEBUG) {
									fmt::print("BGV ERROR: data not purged! Read successful!!\n");
									printGranuleChunks(versionRead2.second);
								}
								ASSERT(false);
							} catch (Error& e) {
								if (e.code() == error_code_actor_cancelled) {
									throw;
								}
								ASSERT(e.code() == error_code_blob_granule_transaction_too_old);
								CODE_PROBE(true, "BGV verified too old after purge");
							}
						}
					}
				}

				// pick a random range
				int rIndex = deterministicRandom()->randomInt(0, self->granuleRanges.get().size());
				state KeyRange range = self->granuleRanges.get()[rIndex];

				state std::pair<RangeResult, Version> fdb = wait(readFromFDB(cx, range));
				std::pair<RangeResult, Standalone<VectorRef<BlobGranuleChunkRef>>> blob =
				    wait(readFromBlob(cx, self->bstore, range, 0, fdb.second));
				if (compareFDBAndBlob(fdb.first, blob, range, fdb.second, BGV_DEBUG)) {
					// TODO: bias for immediately re-reading to catch rollback cases
					double reReadTime = currentTime + deterministicRandom()->random01() * self->timeTravelLimit;
					int memory = fdb.first.expectedSize();
					if (reReadTime <= endTime &&
					    timeTravelChecksMemory + memory <= (self->timeTravelBufferSize / self->threads)) {
						timeTravelChecks[reReadTime] = OldRead(range, fdb.second, fdb.first);
						timeTravelChecksMemory += memory;
					}
				} else {
					self->mismatches++;
				}
				self->rowsRead += fdb.first.size();
				self->bytesRead += fdb.first.expectedSize();
				self->initialReads++;

			} catch (Error& e) {
				if (e.code() == error_code_operation_cancelled) {
					throw;
				}
				if (e.code() != error_code_blob_granule_transaction_too_old && BGV_DEBUG) {
					printf("BGVerifier got unexpected error %s\n", e.name());
				}
				self->errors++;
			}
			// wait(poisson(&last, 5.0));
			wait(poisson(&last, 0.1));
		}
	}

	Future<Void> start(Database const& cx) override {
		clients.reserve(threads + 1);
		clients.push_back(timeout(findGranules(cx, this), testDuration, Void()));
		if (enablePurging && clientId == 0) {
			clients.push_back(
			    timeout(reportErrors(verifyGranules(cx, this, true), "BlobGranuleVerifier"), testDuration, Void()));
		} else if (!enablePurging) {
			for (int i = 0; i < threads; i++) {
				clients.push_back(timeout(
				    reportErrors(verifyGranules(cx, this, false), "BlobGranuleVerifier"), testDuration, Void()));
			}
		}
		return delay(testDuration);
	}

	// handle retries + errors
	// It's ok to reset the transaction here because its read version is only used for reading the granule mapping from
	// the system keyspace
	ACTOR Future<Version> doGrv(Transaction* tr) {
		loop {
			try {
				Version readVersion = wait(tr->getReadVersion());
				return readVersion;
			} catch (Error& e) {
				wait(tr->onError(e));
			}
		}
	}

	ACTOR Future<Void> checkPurgedHistoryEntries(Database cx, BlobGranuleVerifierWorkload* self, KeyRange purgeRange) {
		// quick check to make sure we didn't miss any new granules generated between the purge metadata load time and
		// the actual purge, by checking for any new history keys in the range
		// FIXME: fix this check! The BW granule check is really the important one, this finds occasional leftover
		// metadata from boundary changes that can race with a force purge, but as long as no blob worker acts on the
		// boundary changes, a bit of leftover metadata is a much smaller problem. To confirm that it was a race with
		// force purging, we check that the history version > the force purge version
		state Transaction tr(cx);
		state KeyRange cur = blobGranuleHistoryKeys;
		loop {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			try {
				RangeResult history = wait(tr.getRange(cur, 1000));
				for (auto& it : history) {
					KeyRange keyRange;
					Version version;
					std::tie(keyRange, version) = decodeBlobGranuleHistoryKey(it.key);
					if (purgeRange.intersects(keyRange)) {
						if (BGV_DEBUG && version <= self->forcePurgeVersion) {
							fmt::print("Found range [{0} - {1}) @ {2} that avoided force purge [{3} - {4}) @ {5}!!\n",
							           keyRange.begin.printable(),
							           keyRange.end.printable(),
							           version,
							           purgeRange.begin.printable(),
							           purgeRange.end.printable(),
							           self->forcePurgeVersion);
						}
						ASSERT(version > self->forcePurgeVersion);
					}
					// ASSERT(!purgeRange.intersects(keyRange));
				}
				if (!history.empty() && history.more) {
					cur = KeyRangeRef(keyAfter(history.back().key), cur.end);
				} else {
					break;
				}
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		return Void();
	}

	ACTOR Future<Void> checkPurgedChangeFeeds(Database cx, BlobGranuleVerifierWorkload* self, KeyRange purgeRange) {
		// quick check to make sure we didn't miss any change feeds
		state Transaction tr(cx);
		state KeyRange cur = changeFeedKeys;
		loop {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			try {
				RangeResult feeds = wait(tr.getRange(cur, 1000));
				for (auto& it : feeds) {
					KeyRange keyRange;
					Version version;
					ChangeFeedStatus status;
					std::tie(keyRange, version, status) = decodeChangeFeedValue(it.value);
					if (purgeRange.intersects(keyRange)) {
						Key feedId = it.key.removePrefix(changeFeedKeys.begin);
						if (BGV_DEBUG) {
							fmt::print(
							    "Found Change Feed {0}: [{1} - {2}) that avoided force purge [{3} - {4}) @ {5}!!\n",
							    feedId.printable(),
							    keyRange.begin.printable(),
							    keyRange.end.printable(),
							    purgeRange.begin.printable(),
							    purgeRange.end.printable(),
							    self->forcePurgeVersion);
						}
					}
					ASSERT(!purgeRange.intersects(keyRange));
				}
				if (!feeds.empty() && feeds.more) {
					cur = KeyRangeRef(keyAfter(feeds.back().key), cur.end);
				} else {
					break;
				}
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		return Void();
	}

	ACTOR Future<Void> validateForcePurge(Database cx, BlobGranuleVerifierWorkload* self, KeyRange purgeRange) {
		// FIXME: enable!! right now leaking metadata isn't nearly as bad as leaking data/change feeds
		state bool strictMetadataCheck = false;
		// first, wait for force purge to complete
		if (BGV_DEBUG) {
			fmt::print("BGV waiting for force purge to complete\n");
		}
		wait(cx->waitPurgeGranulesComplete(self->forcePurgeKey.get()));
		if (BGV_DEBUG) {
			fmt::print("BGV force purge completed, checking\n");
		}

		state Transaction tr(cx);

		// check that force purge range is set and that data is not readable
		loop {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			try {
				ForcedPurgeState forcePurgedState = wait(getForcePurgedState(&tr, purgeRange));
				ASSERT(forcePurgedState == ForcedPurgeState::AllPurged);
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		if (BGV_DEBUG) {
			fmt::print("BGV force purge checked state\n");
		}

		// make sure blob read fails
		tr.reset();
		loop {
			try {
				Version readVersion = wait(self->doGrv(&tr));
				wait(success(readFromBlob(cx, self->bstore, purgeRange, 0, readVersion)));
				ASSERT(false);
			} catch (Error& e) {
				if (e.code() == error_code_operation_cancelled) {
					throw e;
				}
				ASSERT(e.code() == error_code_blob_granule_transaction_too_old);
				break;
			}
		}

		if (BGV_DEBUG) {
			fmt::print("BGV force purge checked read\n");
		}

		// check that metadata is gone for each granule
		if (BGV_DEBUG) {
			fmt::print("BGV checking metadata deleted\n");
		}
		state int i;
		state int64_t filesChecked = 0;
		for (i = 0; i < self->purgedDataToCheck.size(); i++) {
			state KeyRange granuleRange = std::get<0>(self->purgedDataToCheck[i]);
			state Version historyVersion = std::get<1>(self->purgedDataToCheck[i]);
			state UID granuleId = std::get<2>(self->purgedDataToCheck[i]);
			state GranuleFiles oldFiles = wait(std::get<3>(self->purgedDataToCheck[i]));
			fmt::print("  Checking [{0} - {1}): {2}\n",
			           granuleRange.begin.printable(),
			           granuleRange.end.printable(),
			           granuleId.toString().substr(0, 6));
			loop {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				try {
					// change feed
					Optional<Value> changeFeed = wait(tr.get(granuleIDToCFKey(granuleId).withPrefix(changeFeedPrefix)));
					ASSERT(!changeFeed.present());

					// file metadata
					RangeResult fileMetadata = wait(tr.getRange(blobGranuleFileKeyRangeFor(granuleId), 1));
					ASSERT(fileMetadata.empty());

					if (strictMetadataCheck) {
						// lock
						Optional<Value> lock = wait(tr.get(blobGranuleLockKeyFor(granuleRange)));
						ASSERT(!lock.present());

						// history entry
						Optional<Value> history = wait(tr.get(blobGranuleHistoryKeyFor(granuleRange, historyVersion)));
						ASSERT(!history.present());

						// split state
						RangeResult splitData = wait(tr.getRange(blobGranuleSplitKeyRangeFor(granuleId), 1));
						ASSERT(splitData.empty());

						// merge state
						Optional<Value> merge = wait(tr.get(blobGranuleMergeKeyFor(granuleId)));
						ASSERT(!merge.present());

						// FIXME: add merge boundaries!
					}

					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}

			// ensure all old files were deleted from blob storage
			state int fileIdx;
			for (fileIdx = 0; fileIdx < oldFiles.snapshotFiles.size() + oldFiles.deltaFiles.size(); fileIdx++) {
				std::string fname = (fileIdx >= oldFiles.snapshotFiles.size())
				                        ? oldFiles.deltaFiles[fileIdx - oldFiles.snapshotFiles.size()].filename
				                        : oldFiles.snapshotFiles[fileIdx].filename;
				state Reference<BackupContainerFileSystem> bstore = self->bstore->getForRead(fname);
				try {
					wait(success(bstore->readFile(fname)));
					ASSERT(false);
				} catch (Error& e) {
					if (e.code() == error_code_operation_cancelled) {
						throw e;
					}
					ASSERT(e.code() == error_code_file_not_found);
					filesChecked++;
				}
			}
		}

		if (BGV_DEBUG) {
			fmt::print("BGV force purge checked {0} old granules and {1} old files cleaned up\n",
			           self->purgedDataToCheck.size(),
			           filesChecked);
		}

		wait(self->checkPurgedHistoryEntries(cx, self, purgeRange));

		if (BGV_DEBUG) {
			fmt::print("BGV force purge checked for new granule history entries\n");
		}

		wait(self->checkPurgedChangeFeeds(cx, self, purgeRange));

		if (BGV_DEBUG) {
			fmt::print("BGV force purge checked for leaked change feeds\n");
		}

		// ask all workers for all of their open granules and make sure none are in the force purge range

		// Because there could be ranges assigned that havne't yet finished opening to check for purge, some BWs might
		// still temporarily have ranges assigned. To address this, we just retry the check after a bit
		loop {
			state bool anyRangesLeft = false;
			state std::vector<BlobWorkerInterface> blobWorkers = wait(getBlobWorkers(cx));

			if (BGV_DEBUG) {
				fmt::print("BGV force purge checking {0} blob worker mappings\n", blobWorkers.size());
			}

			for (i = 0; i < blobWorkers.size(); i++) {
				GetGranuleAssignmentsRequest req;
				req.managerEpoch = -1; // not manager
				Optional<GetGranuleAssignmentsReply> assignments =
				    wait(timeout(brokenPromiseToNever(blobWorkers[i].granuleAssignmentsRequest.getReply(req)),
				                 SERVER_KNOBS->BLOB_WORKER_TIMEOUT));
				if (assignments.present()) {
					for (auto& it : assignments.get().assignments) {
						if (purgeRange.intersects(it.range)) {
							if (BGV_DEBUG) {
								fmt::print("BW {0} still has range [{1} - {2})\n",
								           blobWorkers[i].id().toString(),
								           it.range.begin.printable(),
								           it.range.end.printable());
							}
							anyRangesLeft = true;
						}
					}
				} else {
					if (BGV_DEBUG) {
						fmt::print("BGV mapping check failed to reach BW {0}\n", blobWorkers[i].id().toString());
					}
					// if BW timed out, we don't for sure know it didn't still have some range
					anyRangesLeft = true;
				}
			}
			if (anyRangesLeft) {
				wait(delay(10.0));
			} else {
				break;
			}
		}

		if (BGV_DEBUG) {
			fmt::print("BGV force purge check complete\n");
		}

		return Void();
	}

	ACTOR Future<bool> _check(Database cx, BlobGranuleVerifierWorkload* self) {
		if (self->startedForcePurge) {
			// data may or may not be gone, depending on whether force purge was registered or not. Only do force purge
			// check if we're sure it was registerd, otherwise, only do actual checks if we're sure no force purge was
			// started.
			if (self->forcePurgeKey.present()) {
				wait(self->validateForcePurge(cx, self, normalKeys));
			}
			return true;
		}
		// check error counts, and do an availability check at the end

		if (self->doSetup && self->initAtEnd) {
			// FIXME: this doesn't check the data contents post-conversion, just that it finishes successfully
			wait(self->setUpBlobRange(cx));
		}

		state Transaction tr(cx);
		state Version readVersion = wait(self->doGrv(&tr));
		state Version startReadVersion = readVersion;
		state int checks = 0;

		state KeyRange last;
		state bool availabilityPassed = true;

		state Standalone<VectorRef<KeyRangeRef>> allRanges;
		if (self->granuleRanges.get().empty()) {
			if (BGV_DEBUG) {
				fmt::print("Waiting to get granule ranges for check\n");
			}
			state Future<Void> rangeFetcher = self->findGranules(cx, self);
			loop {
				wait(self->granuleRanges.onChange());
				// wait until entire keyspace has granules
				if (!self->granuleRanges.get().empty()) {
					bool haveAll = true;
					if (self->granuleRanges.get().front().begin != normalKeys.begin ||
					    self->granuleRanges.get().back().end != normalKeys.end) {
						haveAll = false;
					}
					for (int i = 0; haveAll && i < self->granuleRanges.get().size() - 1; i++) {
						if (self->granuleRanges.get()[i].end != self->granuleRanges.get()[i + 1].begin) {
							haveAll = false;
						}
					}
					if (haveAll) {
						break;
					}
				}
			}
			rangeFetcher.cancel();
			if (BGV_DEBUG) {
				fmt::print("Got granule ranges for check\n");
			}
		}
		allRanges = self->granuleRanges.get();
		for (auto& range : allRanges) {
			state KeyRange r = range;
			if (BGV_DEBUG) {
				fmt::print("Final availability check [{0} - {1}) @ {2}\n",
				           r.begin.printable(),
				           r.end.printable(),
				           readVersion);
			}

			try {
				loop {
					try {
						Standalone<VectorRef<BlobGranuleChunkRef>> chunks =
						    wait(tr.readBlobGranules(r, 0, readVersion));
						ASSERT(chunks.size() > 0);
						last = chunks.back().keyRange;
						checks += chunks.size();

						break;
					} catch (Error& e) {
						// it's possible for blob granules to never get opened for the entire test due to fault
						// injection. If we get blob_granule_transaction_too_old, for the latest read version, the
						// granule still needs to open. Wait for that to happen at a higher read version.
						if (e.code() == error_code_blob_granule_transaction_too_old) {
							wait(delay(1.0));
							tr.reset();
							Version rv = wait(self->doGrv(&tr));
							readVersion = rv;
						} else {
							wait(tr.onError(e));
						}
					}
				}
			} catch (Error& e) {
				if (e.code() == error_code_actor_cancelled) {
					throw;
				}
				if (e.code() == error_code_end_of_stream) {
					break;
				}
				if (BGV_DEBUG) {
					fmt::print("BG Verifier failed final availability check for [{0} - {1}) @ {2} with error {3}. Last "
					           "Success=[{4} - {5})\n",
					           r.begin.printable(),
					           r.end.printable(),
					           readVersion,
					           e.name(),
					           last.begin.printable(),
					           last.end.printable());
				}
				availabilityPassed = false;
				break;
			}
		}
		if (BGV_DEBUG && startReadVersion != readVersion) {
			fmt::print("Availability check updated read version from {0} to {1}\n", startReadVersion, readVersion);
		}
		state bool result =
		    availabilityPassed && self->mismatches == 0 && (checks > 0) && (self->timeTravelTooOld == 0);
		fmt::print("Blob Granule Verifier {0} {1}:\n", self->clientId, result ? "passed" : "failed");
		fmt::print("  {} successful final granule checks\n", checks);
		fmt::print("  {} failed final granule checks\n", availabilityPassed ? 0 : 1);
		fmt::print("  {} mismatches\n", self->mismatches);
		fmt::print("  {} time travel too old\n", self->timeTravelTooOld);
		fmt::print("  {} errors\n", self->errors);
		fmt::print("  {} initial reads\n", self->initialReads);
		fmt::print("  {} time travel reads\n", self->timeTravelReads);
		fmt::print("  {} rows\n", self->rowsRead);
		fmt::print("  {} bytes\n", self->bytesRead);
		fmt::print("  {} purges\n", self->purges);
		// FIXME: add above as details to trace event

		TraceEvent("BlobGranuleVerifierChecked").detail("Result", result);

		// For some reason simulation is still passing when this fails?.. so assert for now
		ASSERT(result);

		// FIXME: if doPurging was set, possibly do one last purge here, and verify it succeeds with no errors

		if (self->clientId == 0 && SERVER_KNOBS->BG_ENABLE_MERGING && self->clearAndMergeCheck) {
			CODE_PROBE(true, "BGV clearing database and awaiting merge");
			wait(clearAndAwaitMerge(cx, normalKeys));
		}

		return result;
	}

	Future<bool> check(Database const& cx) override {
		if (clientId == 0 || !doForcePurge) {
			return _check(cx, this);
		}
		return true;
	}
	void getMetrics(std::vector<PerfMetric>& m) override {}
};

WorkloadFactory<BlobGranuleVerifierWorkload> BlobGranuleVerifierWorkloadFactory("BlobGranuleVerifier");
