// This function implements, to our knowledge, the methods decribed in the DENTIST paper
// https://github.com/Yves-CHEN/DENTIST/tree/master#Citations
// Some codes are adapted and rewritten from https://github.com/Yves-CHEN/DENTIST/tree/master
// to fit the Rcpp implementation.
// The code reflects our understanding and interpretation of DENTIST method which may difer in details
// from the author's original proposal, although in various tests we find that our implementation and the
// original results are mostly identical
#include <RcppArmadillo.h>
#include <omp.h> // Required for parallel processing
#include <algorithm>
#include <random>
#include <vector>
#include <numeric> // For std::iota

// Enable C++11 via this plugin (Rcpp 0.10.3 or later)
// [[Rcpp::depends(RcppArmadillo)]]
// [[Rcpp::plugins(cpp11)]]
// [[Rcpp::plugins(openmp)]]

using namespace Rcpp;
using namespace arma;


// Assuming sort_indexes is defined as provided
std::vector<size_t> sort_indexes(const std::vector<int>& v, unsigned int theSize) {
    std::vector<size_t> idx(theSize);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&v](size_t i1, size_t i2) {return v[i1] < v[i2];});
    return idx;
}

// Improved generateSetOfNumbers function using C++11 random
std::vector<size_t> generateSetOfNumbers(int SIZE, int seed) {
    std::vector<int> numbers(SIZE, 0);
    std::mt19937 rng(seed); // Mersenne Twister: Good quality random number generator
    std::uniform_int_distribution<int> dist(0, INT_MAX);

    // Generate the first random number
    numbers[0] = dist(rng);
    for (int index = 1; index < SIZE; index++) {
        int tempNum;
        do {
            tempNum = dist(rng); // Generate a new random number
            // Check for uniqueness in the current list of generated numbers
            bool isUnique = true;
            for (int index2 = 0; index2 < index; index2++) {
                if (tempNum == numbers[index2]) {
                    isUnique = false;
                    break;
                }
            }
            // If the number is not unique, force the loop to try again
            if (!isUnique) tempNum = -1;
        } while (tempNum == -1);
        // Assign the unique number to the list
        numbers[index] = tempNum;
    }

    // Sort the indices of 'numbers' based on their values
    return sort_indexes(numbers, SIZE);
}

// Get a quantile value
double getQuantile(const std::vector<double>& dat, double whichQuantile) {
    std::vector<double> sortedData = dat;
    std::sort(sortedData.begin(), sortedData.end());
    size_t pos = ceil(sortedData.size() * whichQuantile) - 1;
    return sortedData.at(pos);
}

// Get a quantile value based on grouping
double getQuantile2(const std::vector<double>& dat, const std::vector<uint>& grouping, double whichQuantile) {
    std::vector<double> filteredData;
    for (size_t i = 0; i < dat.size(); ++i) if (grouping[i] == 1) filteredData.push_back(dat[i]);
    if (filteredData.size() < 50) return 0;
    return getQuantile(filteredData, whichQuantile);
}

// Calculate minus log p-value of chi-squared statistic
double minusLogPvalueChisq2(double stat) {
    double p = 1.0 - arma::chi2cdf(stat, 1.0);
    return -log10(p);
}

// Perform one iteration of the algorithm, assuming LDmat is an arma::mat
void oneIteration(const arma::mat& LDmat, const std::vector<uint>& idx, const std::vector<uint>& idx2,
                  arma::vec& zScore, arma::vec& imputedZ, arma::vec& rsqList, arma::vec& zScore_e,
                  uint nSample, float probSVD, int ncpus) {
    omp_set_num_threads(ncpus);

    uint K = std::min(static_cast<uint>(idx.size()), nSample) * probSVD;

    arma::mat LD_it(idx2.size(), idx.size());
    arma::vec zScore_eigen(idx.size());
    arma::mat VV(idx.size(), idx.size());

    // Fill LD_it and VV matrices using direct indexing
    #pragma omp parallel for collapse(2)
    for (size_t i = 0; i < idx2.size(); i++) {
        for (size_t k = 0; k < idx.size(); k++) {
            LD_it(i, k) = LDmat(idx2[i], idx[k]);
        }
    }

    #pragma omp parallel for
    for (size_t i = 0; i < idx.size(); i++) {
        zScore_eigen(i) = zScore[idx[i]];
        for (size_t j = 0; j < idx.size(); j++) {
            VV(i, j) = LDmat(idx[i], idx[j]);
        }
    }

    // Eigen decomposition
    arma::vec eigval;
    arma::mat eigvec;
    arma::eig_sym(eigval, eigvec, VV);

    int nRank = eigvec.n_rows;
    int nZeros = arma::sum(eigval < 0.0001);
    nRank -= nZeros;
    K = std::min(K, static_cast<uint>(nRank));
    if (K <= 1) {
        Rcpp::stop("Rank of eigen matrix <= 1");
    }

    arma::mat ui = arma::mat(eigvec.n_rows, K, arma::fill::zeros);
    arma::mat wi = arma::mat(K, K, arma::fill::zeros);
    for (uint m = 0; m < K; ++m) {
        int j = eigvec.n_rows - m - 1;
        ui.col(m) = eigvec.col(j);
        wi(m, m) = 1.0 / eigval(j);
    }

    // Calculate imputed Z scores and R squared values
    arma::mat beta = LD_it * ui * wi;
    arma::vec zScore_eigen_imp = beta * (ui.t() * zScore_eigen);
    arma::vec rsq_eigen = (beta * (ui.t() * LD_it.t())).diag();

    #pragma omp parallel for
    for (size_t i = 0; i < idx2.size(); ++i) {
        imputedZ[idx2[i]] = zScore_eigen_imp(i);
        rsqList[idx2[i]] = rsq_eigen(i);
        if (rsq_eigen(i) >= 1) {
            Rcpp::stop("Dividing zero: Rsq = " + std::to_string(rsq_eigen(i)));
        }
        uint j = idx2[i];
        zScore_e[j] = (zScore[j] - imputedZ[j]) / std::sqrt(LDmat(j, j) - rsqList[j]);
    }
}

/**
 * @brief DENTIST: Detecting Errors in Analyses of Summary Statistics
 *
 * DENTIST (Detecting Errors iN analyses of summary staTISTics) is a 
 * quality control tool for summary-level data from genome-wide association 
 * studies (GWASs). It identifies and removes problematic variants by leveraging 
 * the difference between observed GWAS statistics and predicted values using 
 * linkage disequilibrium data from a reference panel. It is useful for enhancing 
 * the accuracy of various GWAS analyses, including conditional and joint 
 * association analysis, LD score regression, and more.
 *
 * @param LDmat A matrix representing linkage disequilibrium data from a 
 * reference panel. Must be an arma::mat.
 * @param markerSize Total number of markers. Must be an unsigned integer.
 * @param nSample Sample size used in the GWAS. Must be an unsigned integer.
 * @param zScore Vector of GWAS Z-scores. Must be an arma::vec.
 * @param pValueThreshold GWAS P-value threshold for variant filtering. 
 * Must be a double.
 * @param propSVD Proportion of singular value decomposition truncation. 
 * Must be a float.
 * @param gcControl Boolean flag for genetic control adjustment. Must be a boolean.
 * @param nIter Number of iterations for the DENTIST algorithm. Must be an integer.
 * @param groupingPvalue_thresh Threshold for grouping p-values. Must be a double.
 * @param ncpus Number of CPU cores to use for computation. Must be an integer.
 * @param seed Seed for random number generation. Must be an integer.
 *
 * @return Returns a List containing several objects including imputed Z-scores,
 * r-squared values, adjusted Z-scores, iteration IDs, and grouping GWAS results.
 * - imputedZ: Imputed Z-scores for each marker.
 * - rsq: R-squared values for each marker.
 * - zScore_e: Adjusted Z-scores after error detection.
 * - iterID: Iteration ID for each marker indicating the iteration in which
 * the marker passed the QC.
 * - groupingGWAS: Binary vector indicating whether each marker is considered
 * problematic (1) or not (0).
 */
// [[Rcpp::export]]
List dentist(const arma::mat& LDmat, uint markerSize, uint nSample, const arma::vec& zScore,
             double pValueThreshold, float propSVD, bool gcControl, int nIter,
             double groupingPvalue_thresh, int ncpus, int seed) {
    // Set number of threads for parallel processing
    omp_set_num_threads(ncpus);

    // Initialization based on the seed input
    std::vector<size_t> randOrder = generateSetOfNumbers(markerSize, seed);
    std::vector<uint> idx, idx2, fullIdx(randOrder.begin(), randOrder.end());

    // Determining indices for partitioning
    for (uint i = 0; i < markerSize; ++i) {
        if (randOrder[i] > markerSize / 2) idx.push_back(i);
        else idx2.push_back(i);
    }

    std::vector<uint> groupingGWAS(markerSize, 0);
    for (uint i = 0; i < markerSize; ++i) {
        if (minusLogPvalueChisq2(zScore(i) * zScore(i)) > -log10(groupingPvalue_thresh)) {
            groupingGWAS[i] = 1;
        }
    }

    arma::vec imputedZ = arma::zeros<arma::vec>(markerSize);
    arma::vec rsq = arma::zeros<arma::vec>(markerSize);
    arma::vec zScore_e = arma::zeros<arma::vec>(markerSize);
    arma::ivec iterID = arma::zeros<arma::ivec>(markerSize);

    for (int t = 0; t < nIter; ++t) {
        std::vector<uint> idx2_QCed;
        std::vector<double> diff;
        std::vector<uint> grouping_tmp;

        // Perform iteration with current subsets
        oneIteration(LDmat, idx, idx2, zScore, imputedZ, rsq, zScore_e, nSample, propSVD, ncpus);

        // Assess differences and grouping for thresholding
        diff.resize(idx2.size());
        grouping_tmp.resize(idx2.size());
        for (size_t i = 0; i < idx2.size(); ++i) {
            diff[i] = std::abs(zScore_e[idx2[i]]);
            grouping_tmp[i] = groupingGWAS[idx2[i]];
        }

        double threshold = getQuantile(diff, 0.995);
        double threshold1 = getQuantile2(diff, grouping_tmp, 0.995);
        double threshold0 = getQuantile2(diff, !grouping_tmp, 0.995); // Adapt based on the actual logic for inverse grouping

        // Apply threshold-based filtering for QC
        for (size_t i = 0; i < diff.size(); ++i) {
            if ((grouping_tmp[i] == 1 && diff[i] <= threshold1) ||
                (grouping_tmp[i] == 0 && diff[i] <= threshold0)) {
                idx2_QCed.push_back(idx2[i]);
            }
        }

        // Re-evaluate for the next iteration
        oneIteration(LDmat, idx, idx2_QCed, zScore, imputedZ, rsq, zScore_e, nSample, propSVD, ncpus);

        // Prepare indices and data for the next iteration
        std::vector<size_t> fullIdx_tmp;
        for (size_t i : fullIdx) {
            double currentDiff = std::abs(zScore_e[i]);
            if ((groupingGWAS[i] == 1 && currentDiff <= threshold1) ||
                (groupingGWAS[i] == 0 && currentDiff <= threshold0)) {
                fullIdx_tmp.push_back(i);
                iterID[i]++;
            }
        }

        // Adjust for genetic control if necessary
        if (gcControl) {
            // Calculate median chi-squared statistic as an inflation factor
            std::nth_element(chisq.begin(), chisq.begin() + chisq.size() / 2, chisq.end());
            double medianChisq = chisq[chisq.size() / 2];
            double inflationFactor = medianChisq / 0.456; // Example median value for a chi-squared distribution with 1 df

            // Adjust z-scores based on inflation factor
            for (size_t i = 0; i < fullIdx.size(); ++i) {
                if (std::pow(zScore_e[fullIdx[i]], 2) / inflationFactor < pValueThreshold) {
                    // Adjusted condition based on the inflation factor
                    fullIdx_tmp.push_back(fullIdx[i]);
                }
            }
        }

        // Update indices for the next iteration
        fullIdx = fullIdx_tmp;
        randOrder = generateSetOfNumbers(fullIdx.size(), 20000 + t * 20000); // Refresh random order with a new seed
        idx.clear();
        idx2.clear();
        for (uint i : fullIdx) {
            if (randOrder[i] > fullIdx.size() / 2) idx.push_back(i);
            else idx2.push_back(i);
        }
    }

    // Prepare and return results
    return List::create(Named("imputedZ") = imputedZ,
                        Named("rsq") = rsq,
                        Named("zScore_e") = zScore_e,
                        Named("iterID") = iterID,
                        Named("groupingGWAS") = wrap(groupingGWAS));
}