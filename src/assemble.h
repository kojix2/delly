#ifndef ASSEMBLE_H
#define ASSEMBLE_H

#include <boost/progress.hpp>

#include <iostream>
#include "msa.h"
#include "split.h"
#include "gotoh.h"
#include "needle.h"

namespace torali
{
  struct SeqSlice {
    int32_t svid;
    int32_t sstart;
    int32_t inslen;
    int32_t qual;  // Only required for junction count map

    SeqSlice() : svid(-1), sstart(-1), inslen(-1), qual(-1) {}
    SeqSlice(int32_t const sv, int32_t const sst, int32_t const il, int32_t q) : svid(sv), sstart(sst), inslen(il), qual(q) {}
  };


  template<typename TConfig, typename TValidRegion, typename TSRStore>
  inline void
    assemble(TConfig const& c, TValidRegion const& validRegions, std::vector<StructuralVariantRecord>& svs, TSRStore& srStore) {
    // Sequence store
    typedef std::set<std::string> TSequences;
    typedef std::vector<TSequences> TSVSequences;
    TSVSequences seqStore(svs.size(), TSequences());

    // SV consensus done
    std::vector<bool> svcons(svs.size(), false);

    // Open file handles
    typedef std::vector<samFile*> TSamFile;
    typedef std::vector<hts_idx_t*> TIndex;
    TSamFile samfile(c.files.size());
    TIndex idx(c.files.size());
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      samfile[file_c] = sam_open(c.files[file_c].string().c_str(), "r");
      hts_set_fai_filename(samfile[file_c], c.genome.string().c_str());
      idx[file_c] = sam_index_load(samfile[file_c], c.files[file_c].string().c_str());
    }
    bam_hdr_t* hdr = sam_hdr_read(samfile[0]);

    // Parse BAM
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Split-read assembly" << std::endl;
    boost::progress_display show_progress( hdr->n_targets );

    faidx_t* fai = fai_load(c.genome.string().c_str());
    for(int32_t refIndex = 0; refIndex < hdr->n_targets; ++refIndex) {
      ++show_progress;
      if (validRegions[refIndex].empty()) continue;

      // Load sequence
      int32_t seqlen = -1;
      std::string tname(hdr->target_name[refIndex]);
      char* seq = faidx_fetch_seq(fai, tname.c_str(), 0, hdr->target_len[refIndex], &seqlen);
    
      // Collect reads from all samples
      for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
	// Read alignments (full chromosome because primary alignments might be somewhere else)
	hts_itr_t* iter = sam_itr_queryi(idx[file_c], refIndex, 0, hdr->target_len[refIndex]);
	bam1_t* rec = bam_init1();
	while (sam_itr_next(samfile[file_c], iter, rec) >= 0) {
	  // Only primary alignments with the full sequence information
	  if (rec->core.flag & (BAM_FQCFAIL | BAM_FDUP | BAM_FUNMAP | BAM_FSECONDARY | BAM_FSUPPLEMENTARY)) continue;

	  std::size_t seed = hash_lr(rec);
	  if (srStore.find(seed) != srStore.end()) {
	    for(uint32_t ri = 0; ri < srStore[seed].size(); ++ri) {
	      int32_t svid = srStore[seed][ri].svid;
	      //std::cerr << svs[svid].svStart << ',' << svs[svid].svEnd << ',' << svs[svid].svt << ',' << svid << " SV" << std::endl;
	      //std::cerr << seed << '\t' << srStore[seed][ri].svid << '\t' << srStore[seed][ri].sstart << '\t' << srStore[seed][ri].inslen << '\t' << sv[srStore[seed][ri].svid].srSupport << '\t' << sv[srStore[seed][ri].svid].svt << std::endl;

	      if ((!svcons[svid]) && (seqStore[svid].size() < c.maxReadPerSV)) {
		// Get sequence
		std::string sequence;
		sequence.resize(rec->core.l_qseq);
		uint8_t* seqptr = bam_get_seq(rec);
		for (int i = 0; i < rec->core.l_qseq; ++i) sequence[i] = "=ACMGRSVTWYHKDBN"[bam_seqi(seqptr, i)];
		int32_t readlen = sequence.size();

		// Extract subsequence (otherwise MSA takes forever)
		int32_t window = 1000;
		int32_t sPos = srStore[seed][ri].sstart - window;
		int32_t ePos = srStore[seed][ri].sstart + srStore[seed][ri].inslen + window;
		if (rec->core.flag & BAM_FREVERSE) {
		  sPos = (readlen - (srStore[seed][ri].sstart + srStore[seed][ri].inslen)) - window;
		  ePos = (readlen - srStore[seed][ri].sstart) + window;
		}
		if (sPos < 0) sPos = 0;
		if (ePos > (int32_t) readlen) ePos = readlen;
		// Min. seq length and max insertion size, 10kbp?
		if (((ePos - sPos) > window) && ((ePos - sPos) <= (10000 + window))) {
		  std::string seqalign = sequence.substr(sPos, (ePos - sPos));
		  if ((svs[svid].svt == 5) || (svs[svid].svt == 6)) {
		    if (svs[svid].chr == refIndex) reverseComplement(seqalign);
		  }
		  seqStore[svid].insert(seqalign);

		  // Enough split-reads?
		  if ((!_translocation(svs[svid].svt)) && (svs[svid].chr == refIndex)) {
		    if ((seqStore[svid].size() == c.maxReadPerSV) || ((int32_t) seqStore[svid].size() == svs[svid].srSupport)) {
		      bool msaSuccess = false;
		      if (seqStore[svid].size() > 1) {
			//std::cerr << svs[svid].svStart << ',' << svs[svid].svEnd << ',' << svs[svid].svt << ',' << svid << " SV" << std::endl;
			//for(typename TSequences::iterator it = seqStore[svid].begin(); it != seqStore[svid].end(); ++it) std::cerr << *it << std::endl;
			msa(c, seqStore[svid], svs[svid].consensus);
			//outputConsensus(hdr, svs[svid], svs[svid].consensus);
			if ((svs[svid].svt == 1) || (svs[svid].svt == 5)) reverseComplement(svs[svid].consensus);
			//std::cerr << svs[svid].consensus << std::endl;
			if (alignConsensus(c, hdr, seq, NULL, svs[svid])) msaSuccess = true;
			//std::cerr << msaSuccess << std::endl;
		      }
		      if (!msaSuccess) {
			svs[svid].consensus = "";
			svs[svid].srSupport = 0;
			svs[svid].srAlignQuality = 0;
		      }
		      seqStore[svid].clear();
		      svcons[svid] = true;
		    }
		  }
		}
	      }
	    }
	  }
	}
	bam_destroy1(rec);
	hts_itr_destroy(iter);
      }
      // Handle left-overs and translocations
      for(int32_t refIndex2 = 0; refIndex2 <= refIndex; ++refIndex2) {
	char* sndSeq = NULL;
	for(uint32_t svid = 0; svid < svcons.size(); ++svid) {
	  if (!svcons[svid]) {
	    if ((svs[svid].chr != refIndex) || (svs[svid].chr2 != refIndex2)) continue;
	    bool msaSuccess = false;
	    if (seqStore[svid].size() > 1) {
	      // Lazy loading of references
	      if (refIndex != refIndex2) {
		if (sndSeq == NULL) {
		  int32_t seqlen = -1;
		  std::string tname(hdr->target_name[refIndex2]);
		  sndSeq = faidx_fetch_seq(fai, tname.c_str(), 0, hdr->target_len[refIndex2], &seqlen);
		}
	      }
	      //std::cerr << svs[svid].svStart << ',' << svs[svid].svEnd << ',' << svs[svid].svt << ',' << svid << " SV" << std::endl;
	      //for(typename TSequences::iterator it = seqStore[svid].begin(); it != seqStore[svid].end(); ++it) std::cerr << *it << std::endl;
	      msa(c, seqStore[svid], svs[svid].consensus);
	      //outputConsensus(hdr, svs[svid], svs[svid].consensus);
	      if ((svs[svid].svt == 1) || (svs[svid].svt == 5)) reverseComplement(svs[svid].consensus);
	      //std::cerr << "Consensus: " << svs[svid].consensus << std::endl;
	      if (alignConsensus(c, hdr, seq, sndSeq, svs[svid])) msaSuccess = true;
	      //std::cerr << msaSuccess << std::endl;
	    }
	    if (!msaSuccess) {
	      svs[svid].consensus = "";
	      svs[svid].srSupport = 0;
	      svs[svid].srAlignQuality = 0;
	    }
	    seqStore[svid].clear();
	    svcons[svid] = true;
	  }
	}
	if (sndSeq != NULL) free(sndSeq);
      }
      
      // Clean-up
      if (seq != NULL) free(seq);
    }
    // Clean-up
    fai_destroy(fai);
    bam_hdr_destroy(hdr);
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      hts_idx_destroy(idx[file_c]);
      sam_close(samfile[file_c]);
    }
    
    // Clean-up unfinished SVs
    for(uint32_t svid = 0; svid < svcons.size(); ++svid) {
      if (!svcons[svid]) {
	//std::cerr << "Missing: " << svid << ',' << svs[svid].svt << std::endl;
	svs[svid].consensus = "";
	svs[svid].srSupport = 0;
	svs[svid].srAlignQuality = 0;
      }
    }
  }


}

#endif
