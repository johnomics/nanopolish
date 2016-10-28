//---------------------------------------------------------
// Copyright 2015 Ontario Institute for Cancer Research
// Written by Jared Simpson (jared.simpson@oicr.on.ca)
//---------------------------------------------------------
//
// nanopolish_call_variants -- find variants wrt a reference
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <algorithm>
#include <queue>
#include <sstream>
#include <fstream>
#include <set>
#include <omp.h>
#include <getopt.h>
#include <iterator>
#include "htslib/faidx.h"
#include "nanopolish_poremodel.h"
#include "nanopolish_transition_parameters.h"
#include "nanopolish_matrix.h"
#include "nanopolish_klcs.h"
#include "nanopolish_profile_hmm.h"
#include "nanopolish_alignment_db.h"
#include "nanopolish_anchor.h"
#include "nanopolish_fast5_map.h"
#include "nanopolish_variant.h"
#include "nanopolish_haplotype.h"
#include "nanopolish_pore_model_set.h"
#include "nanopolish_duration_model.h"
#include "profiler.h"
#include "progress.h"
#include "stdaln.h"

// Macros
#define max3(x,y,z) std::max(std::max(x,y), z)

// Flags to turn on/off debugging information

//#define DEBUG_HMM_UPDATE 1
//#define DEBUG_HMM_EMISSION 1
//#define DEBUG_TRANSITION 1
//#define DEBUG_PATH_SELECTION 1
//#define DEBUG_SINGLE_SEGMENT 1
//#define DEBUG_SHOW_TOP_TWO 1
//#define DEBUG_SEGMENT_ID 193
//#define DEBUG_BENCHMARK 1

// Hack hack hack
float g_p_skip, g_p_skip_self, g_p_bad, g_p_bad_self;

//
// Getopt
//
#define SUBPROGRAM "variants"

static const char *CONSENSUS_VERSION_MESSAGE =
SUBPROGRAM " Version " PACKAGE_VERSION "\n"
"Written by Jared Simpson.\n"
"\n"
"Copyright 2015 Ontario Institute for Cancer Research\n";

static const char *CONSENSUS_USAGE_MESSAGE =
"Usage: " PACKAGE_NAME " " SUBPROGRAM " [OPTIONS] --reads reads.fa --bam alignments.bam --genome genome.fa\n"
"Find SNPs using a signal-level HMM\n"
"\n"
"  -v, --verbose                        display verbose output\n"
"      --version                        display version\n"
"      --help                           display this help and exit\n"
"      --snps                           only call SNPs\n"
"      --consensus                      run in consensus calling mode\n"
"      --fix-homopolymers               run the experimental homopolymer caller\n"
"  -w, --window=STR                     find variants in window STR (format: ctg:start-end)\n"
"  -r, --reads=FILE                     the 2D ONT reads are in fasta FILE\n"
"  -b, --bam=FILE                       the reads aligned to the reference genome are in bam FILE\n"
"  -e, --event-bam=FILE                 the events aligned to the reference genome are in bam FILE\n"
"  -g, --genome=FILE                    the reference genome is in FILE\n"
"  -o, --outfile=FILE                   write result to FILE [default: stdout]\n"
"  -t, --threads=NUM                    use NUM threads (default: 1)\n"
"  -m, --min-candidate-frequency=F      extract candidate variants from the aligned reads when the variant frequency is at least F (default 0.2)\n"
"  -d, --min-candidate-depth=D          extract candidate variants from the aligned reads when the depth is at least D (default: 20)\n"
"  -c, --candidates=VCF                 read variant candidates from VCF, rather than discovering them from aligned reads\n"
"      --calculate-all-support          when making a call, also calculate the support of the 3 other possible bases\n"
"      --models-fofn=FILE               read alternative k-mer models from FILE\n"
"\nReport bugs to " PACKAGE_BUGREPORT "\n\n";

namespace opt
{
    static unsigned int verbose;
    static std::string reads_file;
    static std::string bam_file;
    static std::string event_bam_file;
    static std::string genome_file;
    static std::string output_file;
    static std::string candidates_file;
    static std::string models_fofn;
    static std::string window;
    static std::string consensus_output;
    static std::string alternative_model_type = DEFAULT_MODEL_TYPE;
    static double min_candidate_frequency = 0.2f;
    static int min_candidate_depth = 20;
    static int calculate_all_support = false;
    static int snps_only = 0;
    static int show_progress = 0;
    static int num_threads = 1;
    static int calibrate = 0;
    static int consensus_mode = 0;
    static int fix_homopolymers = 0;
    static int min_distance_between_variants = 10;
    static int min_flanking_sequence = 30;
    static int max_haplotypes = 1000;
    static int debug_alignments = 0;
}

static const char* shortopts = "r:b:g:t:w:o:e:m:c:d:v";

enum { OPT_HELP = 1,
       OPT_VERSION,
       OPT_VCF,
       OPT_PROGRESS,
       OPT_SNPS_ONLY,
       OPT_CALC_ALL_SUPPORT,
       OPT_CONSENSUS,
       OPT_FIX_HOMOPOLYMERS,
       OPT_MODELS_FOFN,
       OPT_P_SKIP,
       OPT_P_SKIP_SELF,
       OPT_P_BAD,
       OPT_P_BAD_SELF };

static const struct option longopts[] = {
    { "verbose",                 no_argument,       NULL, 'v' },
    { "reads",                   required_argument, NULL, 'r' },
    { "bam",                     required_argument, NULL, 'b' },
    { "event-bam",               required_argument, NULL, 'e' },
    { "genome",                  required_argument, NULL, 'g' },
    { "window",                  required_argument, NULL, 'w' },
    { "outfile",                 required_argument, NULL, 'o' },
    { "threads",                 required_argument, NULL, 't' },
    { "min-candidate-frequency", required_argument, NULL, 'm' },
    { "min-candidate-depth",     required_argument, NULL, 'd' },
    { "candidates",              required_argument, NULL, 'c' },
    { "models-fofn",             required_argument, NULL, OPT_MODELS_FOFN },
    { "p-skip",                  required_argument, NULL, OPT_P_SKIP },
    { "p-skip-self",             required_argument, NULL, OPT_P_SKIP_SELF },
    { "p-bad",                   required_argument, NULL, OPT_P_BAD },
    { "p-bad-self",              required_argument, NULL, OPT_P_BAD_SELF },
    { "consensus",               required_argument, NULL, OPT_CONSENSUS },
    { "fix-homopolymers",        no_argument,       NULL, OPT_FIX_HOMOPOLYMERS },
    { "calculate-all-support",   no_argument,       NULL, OPT_CALC_ALL_SUPPORT },
    { "snps",                    no_argument,       NULL, OPT_SNPS_ONLY },
    { "progress",                no_argument,       NULL, OPT_PROGRESS },
    { "help",                    no_argument,       NULL, OPT_HELP },
    { "version",                 no_argument,       NULL, OPT_VERSION },
    { NULL, 0, NULL, 0 }
};

int get_contig_length(const std::string& contig)
{
    faidx_t *fai = fai_load(opt::genome_file.c_str());
    int len = faidx_seq_len(fai, contig.c_str());
    fai_destroy(fai);
    return len;
}

void annotate_with_all_support(std::vector<Variant>& variants,
                               Haplotype base_haplotype,
                               const std::vector<HMMInputData>& input,
                               const uint32_t alignment_flags)

{
    for(size_t vi = 0; vi < variants.size(); vi++) {

        // Generate a haplotype containing every variant in the set except for vi
        Haplotype test_haplotype = base_haplotype;
        for(size_t vj = 0; vj < variants.size(); vj++) {

            // do not apply the variant we are testing
            if(vj == vi) {
                continue;
            }
            test_haplotype.apply_variant(variants[vj]);
        }

        // Make a vector of four haplotypes, one per base
        std::vector<Haplotype> curr_haplotypes;
        Variant tmp_variant = variants[vi];
        for(size_t bi = 0; bi < 4; ++bi) {
            tmp_variant.alt_seq = "ACGT"[bi];
            Haplotype tmp = test_haplotype;
            tmp.apply_variant(tmp_variant);
            curr_haplotypes.push_back(tmp);
        }

        // Test all reads against the 4 haplotypes
        std::vector<int> support_count(4, 0);

        for(size_t input_idx = 0; input_idx < input.size(); ++input_idx) {
            double best_score = -INFINITY;
            size_t best_hap_idx = 0;

            // calculate which haplotype this read supports best
            for(size_t hap_idx = 0; hap_idx < curr_haplotypes.size(); ++hap_idx) {
                double score = profile_hmm_score(curr_haplotypes[hap_idx].get_sequence(), input[input_idx], alignment_flags);
                if(score > best_score) {
                    best_score = score;
                    best_hap_idx = hap_idx;
                }
            }
            support_count[best_hap_idx] += 1;
        }

        std::stringstream ss;
        for(size_t bi = 0; bi < 4; ++bi) {
            ss << support_count[bi] / (double)input.size() << (bi != 3 ? "," : "");
        }

        variants[vi].add_info("AllSupportFractions", ss.str());
    }
}

std::vector<Variant> generate_candidate_single_base_edits(const AlignmentDB& alignments,
                                                          int region_start,
                                                          int region_end,
                                                          uint32_t alignment_flags)
{
    std::vector<Variant> out_variants;

    std::string contig = alignments.get_region_contig();

    // Add all positively-scoring single-base changes into the candidate set
    for(size_t i = region_start; i < region_end; ++i) {

        for(size_t j = 0; j < 4; ++j) {
            // Substitutions
            Variant v;
            v.ref_name = contig;
            v.ref_position = i;
            v.ref_seq = alignments.get_reference_substring(contig, i, i);
            v.alt_seq = "ACGT"[j];

            if(v.ref_seq != v.alt_seq) {
                out_variants.push_back(v);
            }

            // Insertions
            v.alt_seq = v.ref_seq + "ACGT"[j];
            // ignore insertions of the type "A" -> "AA" as these are redundant
            if(v.alt_seq[1] != v.ref_seq[0]) {
                out_variants.push_back(v);
            }
        }

        // deletion
        Variant del;
        del.ref_name = contig;
        del.ref_position = i - 1;
        del.ref_seq = alignments.get_reference_substring(contig, i - 1, i);
        del.alt_seq = del.ref_seq[0];

        // ignore deletions of the type "AA" -> "A" as these are redundant
        if(del.alt_seq[0] != del.ref_seq[1]) {
            out_variants.push_back(del);
        }
    }
    return out_variants;
}

std::vector<Variant> screen_variants_by_score(const AlignmentDB& alignments,
                                              const std::vector<Variant>& candidate_variants,
                                              uint32_t alignment_flags)
{
    if(opt::verbose > 3) {
        fprintf(stderr, "==== Starting variant screening =====\n");
    }

    std::vector<Variant> out_variants;
    std::string contig = alignments.get_region_contig();
    for(size_t vi = 0; vi < candidate_variants.size(); ++vi) {
        const Variant& v = candidate_variants[vi];

        int calling_start = v.ref_position - opt::min_flanking_sequence;
        int calling_end = v.ref_position + v.ref_seq.size() + opt::min_flanking_sequence;

        Haplotype test_haplotype(contig,
                                 calling_start,
                                 alignments.get_reference_substring(contig, calling_start, calling_end));

        std::vector<HMMInputData> event_sequences =
            alignments.get_event_subsequences(contig, calling_start, calling_end);

        Variant scored_variant = score_variant(v, test_haplotype, event_sequences, alignment_flags);
        scored_variant.info = "";
        if(scored_variant.quality > 0) {
            out_variants.push_back(scored_variant);
        }

        if( (scored_variant.quality > 0 && opt::verbose > 3) || opt::verbose > 5) {
            scored_variant.write_vcf(stderr);
        }
    }
    return out_variants;
}

std::vector<Variant> expand_variants(const AlignmentDB& alignments,
                                     const std::vector<Variant>& candidate_variants,
                                     int region_start,
                                     int region_end,
                                     uint32_t alignment_flags)
{
    std::vector<Variant> out_variants;

    std::string contig = alignments.get_region_contig();

    for(size_t vi = 0; vi < candidate_variants.size(); ++vi) {
        const Variant& in_variant = candidate_variants[vi];

        // add the variant unmodified
        out_variants.push_back(in_variant);

        // don't do anything with substitutions
        if(in_variant.ref_seq.size() == 1 && in_variant.alt_seq.size() == 1) {
            continue;
        }

        // deletion
        Variant v = candidate_variants[vi];
        v.ref_seq = alignments.get_reference_substring(v.ref_name, v.ref_position, v.ref_position + v.ref_seq.size());
        assert(v.ref_seq != candidate_variants[vi].ref_seq);
        assert(v.ref_seq.substr(0, candidate_variants[vi].ref_seq.size()) == candidate_variants[vi].ref_seq);
        out_variants.push_back(v);

        // insertion
        for(size_t j = 0; j < 4; ++j) {
            v = candidate_variants[vi];
            v.alt_seq.append(1, "ACGT"[j]);
            out_variants.push_back(v);
        }
    }
    return out_variants;
}

std::vector<Variant> get_variants_from_vcf(const std::string& filename,
                                           const std::string& contig,
                                           int region_start,
                                           int region_end)
{
    std::vector<Variant> out;
    std::ifstream infile(filename);
    std::string line;
    while(getline(infile, line)) {

        // skip headers
        if(line[0] == '#') {
            continue;
        }

        // parse variant
        Variant v(line);

        if(v.ref_name == contig &&
           (int)v.ref_position >= region_start &&
           (int)v.ref_position <= region_end)
        {
            out.push_back(v);
        }
    }
    return out;
}

void print_debug_stats(const std::string& contig,
                       const int start_position,
                       const int stop_position,
                       const Haplotype& base_haplotype,
                       const Haplotype& called_haplotype,
                       const std::vector<HMMInputData>& event_sequences,
                       uint32_t alignment_flags)
{
    std::stringstream prefix_ss;
    prefix_ss << "variant.debug." << contig << ":" << start_position << "-" << stop_position;
    std::string stats_fn = prefix_ss.str() + ".stats.out";
    std::string alignment_fn = prefix_ss.str() + ".alignment.out";

    FILE* stats_out = fopen(stats_fn.c_str(), "w");
    FILE* alignment_out = fopen(alignment_fn.c_str(), "w");

    for(size_t i = 0; i < event_sequences.size(); i++) {
        const HMMInputData& data = event_sequences[i];

        // summarize score
        double num_events = abs(data.event_start_idx - data.event_stop_idx) + 1;
        double base_score = profile_hmm_score(base_haplotype.get_sequence(), data, alignment_flags);
        double called_score = profile_hmm_score(called_haplotype.get_sequence(), data, alignment_flags);
        double base_avg = base_score / num_events;
        double called_avg = called_score / num_events;
        const PoreModel& pm = data.read->pore_model[data.strand];
        fprintf(stats_out, "%s\t%zu\t%zu\t", data.read->read_name.c_str(), data.strand, data.rc);
        fprintf(stats_out, "%.2lf\t%.2lf\t\t%.2lf\t%.2lf\t%.2lf\t", base_score, called_score, base_avg, called_avg, called_score - base_score);
        fprintf(stats_out, "%.2lf\t%.2lf\t%.4lf\t%.2lf\n", pm.shift, pm.scale, pm.drift, pm.var);

        // print paired alignment
        std::vector<HMMAlignmentState> base_align = profile_hmm_align(base_haplotype.get_sequence(), data, alignment_flags);
        std::vector<HMMAlignmentState> called_align = profile_hmm_align(called_haplotype.get_sequence(), data, alignment_flags);
        size_t k = pm.k;
        size_t bi = 0;
        size_t ci = 0;

        // Find the first event aligned in both
        size_t max_event = std::max(base_align[0].event_idx, called_align[0].event_idx);
        while(bi < base_align.size() && base_align[bi].event_idx != max_event) bi++;
        while(ci < called_align.size() && called_align[ci].event_idx != max_event) ci++;

        GaussianParameters standard_normal(0, 1.0);

        double sum_base_abs_sl = 0.0f;
        double sum_called_abs_sl = 0.0f;
        while(bi < base_align.size() && ci < called_align.size()) {
            size_t event_idx = base_align[bi].event_idx;
            assert(called_align[ci].event_idx == event_idx);

            double event_mean = data.read->get_fully_scaled_level(event_idx, data.strand);
            double event_stdv = data.read->get_stdv(event_idx, data.strand);
            double event_duration = data.read->get_duration(event_idx, data.strand);

            std::string base_kmer = base_haplotype.get_sequence().substr(base_align[bi].kmer_idx, k);
            std::string called_kmer = called_haplotype.get_sequence().substr(called_align[ci].kmer_idx, k);
            if(data.rc) {
                base_kmer = gDNAAlphabet.reverse_complement(base_kmer);
                called_kmer = gDNAAlphabet.reverse_complement(called_kmer);
            }

            PoreModelStateParams base_model = pm.states[pm.pmalphabet->kmer_rank(base_kmer.c_str(), k)];
            PoreModelStateParams called_model = pm.states[pm.pmalphabet->kmer_rank(called_kmer.c_str(), k)];

            float base_standard_level = (event_mean - base_model.level_mean) / (sqrt(pm.var) * base_model.level_stdv);
            float called_standard_level = (event_mean - called_model.level_mean) / (sqrt(pm.var) * called_model.level_stdv);
            base_standard_level = base_align[bi].state == 'M' ? base_standard_level : INFINITY;
            called_standard_level = called_align[ci].state == 'M' ? called_standard_level : INFINITY;

            sum_base_abs_sl = base_align[bi].l_fm;
            sum_called_abs_sl = called_align[bi].l_fm;

            char diff = base_kmer != called_kmer ? 'D' : ' ';
            fprintf(alignment_out, "%s\t%zu\t%.2lf\t%.2lf\t%.4lf\t", data.read->read_name.c_str(), event_idx, event_mean, event_stdv, event_duration);
            fprintf(alignment_out, "%c\t%c\t%zu\t%zu\t\t", base_align[bi].state, called_align[ci].state, base_align[bi].kmer_idx, called_align[ci].kmer_idx);
            fprintf(alignment_out, "%s\t%.2lf\t%s\t%.2lf\t", base_kmer.c_str(), base_model.level_mean, called_kmer.c_str(), called_model.level_mean);
            fprintf(alignment_out, "%.2lf\t%.2lf\t%c\t%.2lf\n", base_standard_level, called_standard_level, diff, sum_called_abs_sl - sum_base_abs_sl);

            // Go to the next event
            while(base_align[bi].event_idx == event_idx) bi++;
            while(called_align[ci].event_idx == event_idx) ci++;
        }
    }

    fclose(stats_out);
    fclose(alignment_out);
}

Haplotype fix_homopolymers(const Haplotype& input_haplotype,
                           const AlignmentDB& alignments)
{
    uint32_t alignment_flags = 0;
    Haplotype fixed_haplotype = input_haplotype;
    const std::string& haplotype_sequence = input_haplotype.get_sequence();
    size_t kmer_size = 6;
    size_t MIN_HP_LENGTH = 3;
    size_t MAX_HP_LENGTH = 9;
    double CALL_THRESHOLD = 10;

    // scan for homopolymers
    size_t i = 0;
    while(i < haplotype_sequence.size()) {
        // start a new homopolymer
        char hp_base = haplotype_sequence[i];
        size_t hap_hp_start = i;
        size_t ref_hp_start = hap_hp_start + input_haplotype.get_reference_position();
        while(i < haplotype_sequence.size() && haplotype_sequence[i] == hp_base) i++;

        if(i >= haplotype_sequence.size()) {
            break;
        }

        size_t hap_hp_end = i;
        size_t hp_length = hap_hp_end - hap_hp_start;

        if(hp_length < MIN_HP_LENGTH || hp_length > MAX_HP_LENGTH)
            continue;

        // Set the calling range based on the *reference* (not haplotype) coordinates
        // of the region surrounding the homopolymer. This is so we can extract the alignments
        // from the alignment DB using reference coordinates. NB get_enclosing... may change
        // hap_calling_start/end
        if(hap_hp_start < opt::min_flanking_sequence)
            continue;
        if(hap_hp_end + opt::min_flanking_sequence >= haplotype_sequence.size())
            continue;

        size_t hap_calling_start = hap_hp_start - opt::min_flanking_sequence;
        size_t hap_calling_end = hap_hp_end + opt::min_flanking_sequence;
        size_t ref_calling_start, ref_calling_end;
        input_haplotype.get_enclosing_reference_range_for_haplotype_range(hap_calling_start,
                                                                          hap_calling_end,
                                                                          ref_calling_start,
                                                                          ref_calling_end);

        if(ref_calling_start == std::string::npos || ref_calling_end == std::string::npos) {
            continue;
        }

        if(opt::verbose > 3) {
            fprintf(stderr, "[fixhp] Found %zu-mer %c at %zu (seq: %s)\n", hp_length, hp_base, hap_hp_start, haplotype_sequence.substr(hap_hp_start - kmer_size - 1, hp_length + 10).c_str());
        }

        if(ref_calling_start < alignments.get_region_start() || ref_calling_end >= alignments.get_region_end()) {
            continue;
        }
        assert(ref_calling_start <= ref_calling_end);

        if(ref_calling_start < input_haplotype.get_reference_position() ||
           ref_calling_end >= input_haplotype.get_reference_end()) {
            continue;
        }

        Haplotype calling_haplotype =
            input_haplotype.substr_by_reference(ref_calling_start, ref_calling_end);
        std::string calling_sequence = calling_haplotype.get_sequence();

        // Get the events for the calling region
        std::vector<HMMInputData> event_sequences =
            alignments.get_event_subsequences(alignments.get_region_contig(), ref_calling_start, ref_calling_end);

        // the kmer with the first base of the homopolymer in the last position
        size_t k0 = hap_hp_start - hap_calling_start - kmer_size + 1;

        // the kmer with the last base of the homopolymer in the first position
        size_t k1 = hap_hp_end - hap_calling_start;

        std::string key_str = haplotype_sequence.substr(hap_hp_start - kmer_size - 1, hp_length + 10);
        std::vector<double> duration_likelihoods(MAX_HP_LENGTH + 1, 0.0f);
        std::vector<double> event_likelihoods(MAX_HP_LENGTH + 1, 0.0f);

        for(size_t j = 0; j < event_sequences.size(); ++j) {
            assert(kmer_size == event_sequences[j].read->pore_model[0].k);
            assert(kmer_size == 6);
            
            const SquiggleRead* read = event_sequences[j].read;
            size_t strand = event_sequences[j].strand;

            // skip small event regions
            if( abs(event_sequences[j].event_start_idx - event_sequences[j].event_stop_idx) < 10) {
                continue;
            }
            

            // Fit a gamma distribution to the durations in this region of the read
            double local_time = fabs(read->get_time(event_sequences[j].event_start_idx, strand) - read->get_time(event_sequences[j].event_stop_idx, strand));
            double local_bases = calling_sequence.size();
            double local_avg = local_time / local_bases;
            GammaParameters params;
            params.shape = 2.461964;
            params.rate = (1 / local_avg) * params.shape;
            if(opt::verbose > 3) {
                fprintf(stderr, "[fixhp] RATE local:  %s\t%.6lf\n", read->read_name.c_str(), local_avg);
            }

            // Calculate the duration likelihood of an l-mer at this hp
            // we align to a modified version of the haplotype sequence which contains the l-mer
            for(int var_sequence_length = MIN_HP_LENGTH; var_sequence_length <= MAX_HP_LENGTH; ++var_sequence_length) {
                int var_sequence_diff = var_sequence_length - hp_length;
                std::string variant_sequence = calling_sequence;
                if(var_sequence_diff < 0) {
                    variant_sequence.erase(hap_hp_start - hap_calling_start, abs(var_sequence_diff));
                } else if(var_sequence_diff > 0) {
                    variant_sequence.insert(hap_hp_start - hap_calling_start, var_sequence_diff, hp_base);
                }

                // align events
                std::vector<double> durations_by_kmer = DurationModel::generate_aligned_durations(variant_sequence,
                                                                                                  event_sequences[j],
                                                                                                  alignment_flags);
                // event current measurement likelihood using the standard HMM
                event_likelihoods[var_sequence_length] += profile_hmm_score(variant_sequence, event_sequences[j], alignment_flags);

                // the call window parameter determines how much flanking sequence around the HP we include in the total duration calculation
                int call_window = 2;
                size_t variant_offset_start = k0 + 4 - call_window;
                size_t variant_offset_end = k0 + hp_length + var_sequence_diff + call_window;
                double sum_duration = 0.0f;
                for(size_t k = variant_offset_start; k < variant_offset_end; k++) {
                    sum_duration += durations_by_kmer[k];
                }

                double num_kmers = variant_offset_end - variant_offset_start;
                double log_gamma = sum_duration > MIN_DURATION ?  DurationModel::log_gamma_sum(sum_duration, params, num_kmers) : 0.0f;
                if(read->pore_model[strand].metadata.kit == KV_SQK007) {
                    duration_likelihoods[var_sequence_length] += log_gamma;
                }
                if(opt::verbose > 3) {
                   fprintf(stderr, "SUM_VAR\t%zu\t%d\t%d\t%d\t%d\t%.5lf\t%.2lf\n", ref_hp_start, hp_length, var_sequence_length, call_window, variant_offset_end - variant_offset_start, sum_duration, log_gamma);
                }
            }
        }

        std::stringstream duration_lik_out;
        std::stringstream event_lik_out;
        std::vector<double> score_by_length(duration_likelihoods.size());

        // make a call
        double max_score = -INFINITY;
        size_t call = -1;

        for(size_t len = MIN_HP_LENGTH; len <= MAX_HP_LENGTH; ++len) {
            assert(len < duration_likelihoods.size());
            double d_lik = duration_likelihoods[len];
            double e_lik = event_likelihoods[len];

            double score = d_lik + e_lik;
            score_by_length[len] = score;
            if(score > max_score) {
                max_score = score;
                call = len;
            }
            duration_lik_out << d_lik << "\t";
            event_lik_out << e_lik << "\t";
        }

        double score = max_score - score_by_length[hp_length];
        if(opt::verbose > 3) {
            double del_score = duration_likelihoods[hp_length - 1] - duration_likelihoods[hp_length];
            double ins_score = duration_likelihoods[hp_length + 1] - duration_likelihoods[hp_length];
            double del_e_score = event_likelihoods[hp_length - 1] - event_likelihoods[hp_length];
            double ins_e_score = event_likelihoods[hp_length + 1] - event_likelihoods[hp_length];
            fprintf(stderr, "CALL\t%zu\t%.2lf\n", call, score);
            fprintf(stderr, "LIKELIHOOD\t%s\n", duration_lik_out.str().c_str());
            fprintf(stderr, "EIKELIHOOD\t%s\n", event_lik_out.str().c_str());
            fprintf(stderr, "REF_SCORE\t%zu\t%zu\t%.2lf\t%.2lf\n", ref_hp_start, hp_length, del_score, ins_score);
            fprintf(stderr, "EVENT_SCORE\t%zu\t%zu\t%.2lf\t%.2lf\n", ref_hp_start, hp_length, del_e_score, ins_e_score);
            fprintf(stderr, "COMBINED_SCORE\t%zu\t%zu\t%.2lf\t%.2lf\n", ref_hp_start, hp_length, del_score + del_e_score, ins_score + ins_e_score);
        }

        if(score < CALL_THRESHOLD)
            continue;

        int size_diff = call - hp_length;
        std::string contig = fixed_haplotype.get_reference_name();
        if(size_diff > 0) {
            // add a 1bp insertion in this region
            // the variant might conflict with other variants in the region
            // so we try multiple positions
            // NB: it is intended that if the call is a 2bp (or greater) insertion
            // we only insert 1bp (for now)
            for(size_t k = hap_hp_start; k <= hap_hp_end; ++k) {
                Variant v;
                v.ref_name = contig;
                v.ref_position = input_haplotype.get_reference_position_for_haplotype_base(k);
                if(v.ref_position == std::string::npos) {
                    continue;
                }
                v.ref_seq = fixed_haplotype.substr_by_reference(v.ref_position, v.ref_position).get_sequence();
                if(v.ref_seq.size() == 1 && v.ref_seq[0] == hp_base) {
                    v.alt_seq = v.ref_seq + hp_base;
                    v.quality = score;
                    // if the variant can be added here (ie it doesnt overlap a
                    // conflicting variant) then stop
                    if(fixed_haplotype.apply_variant(v)) {
                        break;
                    }
                }
            }
        } else if(size_diff < 0) {
            // add a 1bp deletion at this position
            for(size_t k = hap_hp_start; k <= hap_hp_end; ++k) {
                Variant v;
                v.ref_name = contig;
                v.ref_position = input_haplotype.get_reference_position_for_haplotype_base(k);
                v.quality = score;
                if(v.ref_position == std::string::npos) {
                    continue;
                }
                v.ref_seq = fixed_haplotype.substr_by_reference(v.ref_position, v.ref_position + 1).get_sequence();
                if(v.ref_seq.size() == 2 && v.ref_seq[0] == hp_base && v.ref_seq[1] == hp_base) {
                    v.alt_seq = v.ref_seq[0];

                    // if the variant can be added here (ie it doesnt overlap a
                    // conflicting variant) then stop
                    if(fixed_haplotype.apply_variant(v)) {
                        break;
                    }
                }
            }
        }
    }

    return fixed_haplotype;
}

Haplotype call_haplotype_from_candidates(const AlignmentDB& alignments,
                                         const std::vector<Variant>& candidate_variants,
                                         uint32_t alignment_flags)
{
    Haplotype derived_haplotype(alignments.get_region_contig(), alignments.get_region_start(), alignments.get_reference());

    size_t curr_variant_idx = 0;
    while(curr_variant_idx < candidate_variants.size()) {

        // Group the variants that are within calling_span bases of each other
        size_t end_variant_idx = curr_variant_idx + 1;
        while(end_variant_idx < candidate_variants.size()) {
            int distance = candidate_variants[end_variant_idx].ref_position -
                           candidate_variants[end_variant_idx - 1].ref_position;
            if(distance > opt::min_distance_between_variants)
                break;
            end_variant_idx++;
        }

        size_t num_variants = end_variant_idx - curr_variant_idx;
        int calling_start = candidate_variants[curr_variant_idx].ref_position - opt::min_flanking_sequence;
        int calling_end = candidate_variants[end_variant_idx - 1].ref_position +
                          candidate_variants[end_variant_idx - 1].ref_seq.length() +
                          opt::min_flanking_sequence;
        int calling_size = calling_end - calling_start;

        if(opt::verbose > 2) {
            fprintf(stderr, "%zu variants in span [%d %d]\n", num_variants, calling_start, calling_end);
        }

        // Only try to call if the window is not too large
        if(calling_size <= 200) {

            // Subset the haplotype to the region we are calling
            Haplotype calling_haplotype =
                derived_haplotype.substr_by_reference(calling_start, calling_end);

            // Get the events for the calling region
            std::vector<HMMInputData> event_sequences =
                alignments.get_event_subsequences(alignments.get_region_contig(), calling_start, calling_end);

            // Subset the variants
            std::vector<Variant> calling_variants(candidate_variants.begin() + curr_variant_idx,
                                                  candidate_variants.begin() + end_variant_idx);

            // Select the best set of variants
            std::vector<Variant> selected_variants =
                select_variant_set(calling_variants, calling_haplotype, event_sequences, opt::max_haplotypes, alignment_flags);

            // optionally annotate each variant with fraction of reads supporting A,C,G,T at this position
            if(opt::calculate_all_support) {
                annotate_with_all_support(selected_variants, calling_haplotype, event_sequences, alignment_flags);
            }

            // Apply them to the final haplotype
            for(size_t vi = 0; vi < selected_variants.size(); vi++) {

                derived_haplotype.apply_variant(selected_variants[vi]);

                if(opt::verbose > 1) {
                    selected_variants[vi].write_vcf(stderr);
                }
            }

            if(opt::debug_alignments) {
                print_debug_stats(alignments.get_region_contig(),
                                  calling_start,
                                  calling_end,
                                  calling_haplotype,
                                  derived_haplotype.substr_by_reference(calling_start, calling_end),
                                  event_sequences,
                                  alignment_flags);
            }
        } else {
            fprintf(stderr, "Warning: %zu variants in span, region not called [%d %d]\n", num_variants, calling_start, calling_end);
		}

        // advance to start of next region
        curr_variant_idx = end_variant_idx;
    }

    return derived_haplotype;
}


Haplotype call_variants_for_region(const std::string& contig, int region_start, int region_end)
{
    const int BUFFER = opt::min_flanking_sequence + 10;
    uint32_t alignment_flags = HAF_ALLOW_PRE_CLIP | HAF_ALLOW_POST_CLIP;

    // load the region, accounting for the buffering
    if(region_start < BUFFER)
        region_start = BUFFER;
    AlignmentDB alignments(opt::reads_file, opt::genome_file, opt::bam_file, opt::event_bam_file, opt::calibrate);

    if(!opt::alternative_model_type.empty()) {
        alignments.set_alternative_model_type(opt::alternative_model_type);
    }

    alignments.load_region(contig, region_start - BUFFER, region_end + BUFFER);

    // if the end of the region plus the buffer sequence goes past
    // the end of the chromosome, we adjust the region end here
    region_end = alignments.get_region_end() - BUFFER;

    if(opt::verbose > 4) {
        fprintf(stderr, "input region: %s\n", alignments.get_reference_substring(contig, region_start - BUFFER, region_end + BUFFER).c_str());
    }

/*
    Haplotype called_haplotype(alignments.get_region_contig(),
                               alignments.get_region_start(),
                               alignments.get_reference());
*/

    // Step 1. Discover putative variants across the whole region
    std::vector<Variant> candidate_variants;
    if(opt::candidates_file.empty()) {
        candidate_variants = alignments.get_variants_in_region(contig, region_start, region_end, opt::min_candidate_frequency, opt::min_candidate_depth);
    } else {
        candidate_variants = get_variants_from_vcf(opt::candidates_file, contig, region_start, region_end);
    }

    if(opt::consensus_mode) {

        // generate single-base edits that have a positive haplotype score
        std::vector<Variant> single_base_edits = generate_candidate_single_base_edits(alignments, region_start, region_end, alignment_flags);

        // insert these into the candidate set
        candidate_variants.insert(candidate_variants.end(), single_base_edits.begin(), single_base_edits.end());

        // deduplicate variants
        std::set<Variant, VariantKeyComp> dedup_set(candidate_variants.begin(), candidate_variants.end());
        candidate_variants.clear();
        candidate_variants.insert(candidate_variants.end(), dedup_set.begin(), dedup_set.end());
        std::sort(candidate_variants.begin(), candidate_variants.end(), sortByPosition);
    }

    // Step 2. Call variants

    // in consensus mode we iterate until a maximum number of rounds is reached
    // or the variant set converges
    size_t round = 0;
    size_t MAX_ROUNDS = 5;
    Haplotype called_haplotype(alignments.get_region_contig(),
                               alignments.get_region_start(),
                               alignments.get_reference());
    // Calling strategy in consensus mode
    while(opt::consensus_mode && round++ < MAX_ROUNDS) {
        assert(opt::consensus_mode);
        if(opt::verbose > 3) {
            fprintf(stderr, "Round %zu\n", round);
        }

        // Filter the variant set down by only including those that individually contribute a positive score
        std::vector<Variant> filtered_variants = screen_variants_by_score(alignments,
                                                                          candidate_variants,
                                                                          alignment_flags);

        // Combine variants into sets that maximize their haplotype score
        called_haplotype = call_haplotype_from_candidates(alignments,
                                                          filtered_variants,
                                                          alignment_flags);

        if(opt::consensus_mode) {
            // Expand the called variant set by adding nearby variants
            std::vector<Variant> called_variants = called_haplotype.get_variants();
            candidate_variants = expand_variants(alignments,
                                                 called_variants,
                                                 region_start,
                                                 region_end,
                                                 alignment_flags);
        }
    }

    // Calling strategy in reference-based variant calling mode
    if(!opt::consensus_mode) {
        called_haplotype = call_haplotype_from_candidates(alignments,
                                                          candidate_variants,
                                                          alignment_flags);
    }

    if(opt::fix_homopolymers) {
        called_haplotype = fix_homopolymers(called_haplotype, alignments);
    }

    if(opt::consensus_mode) {
        FILE* consensus_fp = fopen(opt::consensus_output.c_str(), "w");
        fprintf(consensus_fp, ">%s:%d-%d\n%s\n", contig.c_str(),
                                  alignments.get_region_start(),
                                  alignments.get_region_end(),
                                  called_haplotype.get_sequence().c_str());
        fclose(consensus_fp);
    }

    return called_haplotype;
}

void parse_call_variants_options(int argc, char** argv)
{
    bool die = false;
    for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) {
        std::istringstream arg(optarg != NULL ? optarg : "");
        switch (c) {
            case 'r': arg >> opt::reads_file; break;
            case 'g': arg >> opt::genome_file; break;
            case 'b': arg >> opt::bam_file; break;
            case 'e': arg >> opt::event_bam_file; break;
            case 'w': arg >> opt::window; break;
            case 'o': arg >> opt::output_file; break;
            case 'm': arg >> opt::min_candidate_frequency; break;
            case 'd': arg >> opt::min_candidate_depth; break;
            case 'c': arg >> opt::candidates_file; break;
            case '?': die = true; break;
            case 't': arg >> opt::num_threads; break;
            case 'v': opt::verbose++; break;
            case OPT_CONSENSUS: arg >> opt::consensus_output; opt::consensus_mode = 1; break;
            case OPT_FIX_HOMOPOLYMERS: opt::fix_homopolymers = 1; break;
            case OPT_MODELS_FOFN: arg >> opt::models_fofn; break;
            case OPT_CALC_ALL_SUPPORT: opt::calculate_all_support = 1; break;
            case OPT_SNPS_ONLY: opt::snps_only = 1; break;
            case OPT_PROGRESS: opt::show_progress = 1; break;
            case OPT_P_SKIP: arg >> g_p_skip; break;
            case OPT_P_SKIP_SELF: arg >> g_p_skip_self; break;
            case OPT_P_BAD: arg >> g_p_bad; break;
            case OPT_P_BAD_SELF: arg >> g_p_bad_self; break;
            case OPT_HELP:
                std::cout << CONSENSUS_USAGE_MESSAGE;
                exit(EXIT_SUCCESS);
            case OPT_VERSION:
                std::cout << CONSENSUS_VERSION_MESSAGE;
                exit(EXIT_SUCCESS);
        }
    }

    if (argc - optind < 0) {
        std::cerr << SUBPROGRAM ": missing arguments\n";
        die = true;
    } else if (argc - optind > 0) {
        std::cerr << SUBPROGRAM ": too many arguments\n";
        die = true;
    }

    if(opt::num_threads <= 0) {
        std::cerr << SUBPROGRAM ": invalid number of threads: " << opt::num_threads << "\n";
        die = true;
    }

    if(opt::reads_file.empty()) {
        std::cerr << SUBPROGRAM ": a --reads file must be provided\n";
        die = true;
    }

    if(opt::genome_file.empty()) {
        std::cerr << SUBPROGRAM ": a --genome file must be provided\n";
        die = true;
    }

    if(opt::bam_file.empty()) {
        std::cerr << SUBPROGRAM ": a --bam file must be provided\n";
        die = true;
    }

    if(opt::models_fofn.empty()) {
        std::cerr << SUBPROGRAM ": a --models file must be provided\n";
        die = true;
    } else {
        // initialize the model set from the fofn
        PoreModelSet::initialize(opt::models_fofn);
    }

    if (die)
    {
        std::cout << "\n" << CONSENSUS_USAGE_MESSAGE;
        exit(EXIT_FAILURE);
    }
}

int call_variants_main(int argc, char** argv)
{
    parse_call_variants_options(argc, argv);
    omp_set_num_threads(opt::num_threads);

    // Parse the window string
    // Replace ":" and "-" with spaces to make it parseable with stringstream
    std::replace(opt::window.begin(), opt::window.end(), ':', ' ');
    std::replace(opt::window.begin(), opt::window.end(), '-', ' ');

    std::stringstream parser(opt::window);
    std::string contig;
    int start_base;
    int end_base;

    parser >> contig >> start_base >> end_base;
    end_base = std::min(end_base, get_contig_length(contig) - 1);

    FILE* out_fp;
    if(!opt::output_file.empty()) {
        out_fp = fopen(opt::output_file.c_str(), "w");
    } else {
        out_fp = stdout;
    }

    Variant::write_vcf_header(out_fp);

    fprintf(stderr, "TODO: train model\n");
    fprintf(stderr, "TODO: filter data\n");

    Haplotype haplotype = call_variants_for_region(contig, start_base, end_base);

    std::vector<Variant> variants = haplotype.get_variants();
    for(size_t vi = 0; vi < variants.size(); vi++) {
        variants[vi].write_vcf(out_fp);
    }

    if(out_fp != stdout) {
        fclose(out_fp);
    }
    return 0;
}
