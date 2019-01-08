#ifndef itkRandomForest_txx
#define itkRandomForest_txx

//#include "itkRandomForest.h"

#include "itkObjectFactory.h"
#include "itkImageRegionConstIterator.h"
#include "itkImageRegionIterator.h"
#include <math.h>
#include <algorithm>

namespace itk
{

template< class TInputImage>
RandomForest<TInputImage>
::RandomForest()
{
    this->SetNumberOfRequiredInputs(2);
}

template< class TInputImage>
void
RandomForest<TInputImage>
::SetInputImage(const TInputImage* image)
{
    this->SetNthInput(0, const_cast<TInputImage*>(image));
}

template< class TInputImage>
void
RandomForest<TInputImage>
::SetLabelMap(const TInputImage* mask)
{
    this->SetNthInput(1, const_cast<TInputImage*>(mask));
}

template< class TInputImage>
typename TInputImage::Pointer RandomForest<TInputImage>::GetInputImage()
{
    return static_cast< TInputImage * >
            ( this->ProcessObject::GetInput(0) );
}

template< class TInputImage>
typename TInputImage::Pointer RandomForest<TInputImage>::GetLabelMap()
{
    return static_cast< TInputImage * >
            ( this->ProcessObject::GetInput(1) );
}

template< class TInputImage>
void
RandomForest<TInputImage>
::GenerateData()
{

    typename TInputImage::Pointer intensity_image = this->GetInputImage();
    typename TInputImage::Pointer label_image = this->GetLabelMap();

    // Setup output 1
    typename TInputImage::Pointer output = this->GetOutput();
    output ->SetBufferedRegion(output->GetRequestedRegion());
    output ->Allocate();

    std::string rf_file = "myforest.rf";
    const char * train_file = rf_file.c_str();
    const int VDim = 3;
    RFParameters<TPixel, VDim> param;

    // Get the segmentation image - which determines the samples
    typedef itk::ImageRegionConstIteratorWithIndex<TInputImage> LabelIter;

    // Shrink the buffered region by radius because we can't handle BCs
    itk::ImageRegion<VDim> reg = label_image->GetBufferedRegion();
    reg.ShrinkByRadius(param.patch_radius);

    // We need to iterate throught the label image once to determine the
    // number of samples to allocate.
    unsigned long nSamples = 0;
    for(LabelIter lit(label_image, reg); !lit.IsAtEnd(); ++lit)
        if( (int) (0.5 + lit.Value()) > 0)
            nSamples++;

    // Create an iterator for going over all the anatomical image data
    CollectionIter cit(reg);
    param.patch_radius.Fill(2); // Use a neighborhood patch for features
    cit.SetRadius(param.patch_radius);

    // Add all the anatomical images to this iterator
    cit.AddImage(intensity_image);

    // Get the number of components
    int nComp = cit.GetTotalComponents();
    int nPatch = cit.GetNeighborhoodSize();
    int nColumns = nComp * nPatch;

    // Are we using coordinate informtion
    if(param.use_coordinate_features)
        nColumns += VDim;

    // Create a new sample
    typedef MLData<TPixel, TPixel> SampleType;
    SampleType *sample = new SampleType(nSamples, nColumns);

    // Now fill out the samples
    int iSample = 0;
    for(LabelIter lit(label_image, reg); !lit.IsAtEnd(); ++lit, ++cit)
    {
        int label = (int) (lit.Value() + 0.5);
        if(label > 0)
        {
            // Fill in the data
            std::vector<TPixel> &column = sample->data[iSample];
            int k = 0;
            for(int i = 0; i < nComp; i++)
                for(int j = 0; j < nPatch; j++)
                    column[k++] = cit.NeighborValue(i,j);

            // Add the coordinate features if used
            if(param.use_coordinate_features)
                for(int d = 0; d < VDim; d++)
                    column[k++] = lit.GetIndex()[d];

            // Fill in the label
            sample->label[iSample] = label;
            ++iSample;
        }
    }

    // Check that the sample has at least two distinct labels
    bool isValidSample = false;
    for(int iSample = 1; iSample < sample->Size(); iSample++)
        if(sample->label[iSample] != sample->label[iSample-1])
        { isValidSample = true; break; }

    // Set up the classifier parameters
    TrainingParameters params;

    // TODO:
    params.treeDepth = param.tree_depth;
    params.treeNum = param.forest_size;
    params.candidateNodeClassifierNum = 10;
    params.candidateClassifierThresholdNum = 10;
    params.subSamplePercent = 0;
    params.splitIG = 0.1;
    params.leafEntropy = 0.05;
    params.verbose = true;

    // Cap the number of training voxels at some reasonable number
    if(sample->Size() > 10000)
        params.subSamplePercent = 100 * 10000.0 / sample->Size();
    else
        params.subSamplePercent = 0;

    // Create the classification engine
    typedef typename RFClassifierType::RFAxisClassifierType RFAxisClassifierType;
    typedef Classification<TPixel, TPixel, RFAxisClassifierType> ClassificationType;

    typename RFClassifierType::Pointer classifier = RFClassifierType::New();
    ClassificationType classification;

    // Perform classifier training
    classification.Learning(
                params, *sample,
                *classifier->GetForest(),
                classifier->GetValidLabel(),
                classifier->GetClassToLabelMapping());

    // Reset the class weights to the number of classes and assign default
    int n_classes = classifier->GetClassToLabelMapping().size(), n_fore = 0, n_back = 0;
    classifier->GetClassWeights().resize(n_classes, -1.0);

    // Store the patch radius in the classifier - this remains fixed until
    // training is repeated
    classifier->SetPatchRadius(param.patch_radius);
    classifier->SetUseCoordinateFeatures(param.use_coordinate_features);

    // Dump the classifier to a file
    std::ofstream out_file(train_file);
    classifier->Write(out_file);
    out_file.close();

    /** Apply classifier */
    std::cout << "applying classifier" << std::endl;

    // Apply bounding box and input cropped image into the random forest classifier
    typename T3DintImage::SizeType bbox_size = m_boundingbox.GetSize();

    // Set up requested region
    typename TInputImage::SizeType regionSize;
    regionSize[0] = 1;
    regionSize[1] = bbox_size[1];
    regionSize[2] = bbox_size[2];

    typename T3DintImage::IndexType bbox_index = m_boundingbox.GetIndex();

    // Define the random forest classification filter
    typedef RandomForestClassifyImageFilter<TInputImage, VectorImageType, TInputImage, TPixel> FilterType;

    // Create the filter for this label (TODO: this is wasting computation)
    typename FilterType::Pointer filter = FilterType::New();

    if(m_intermediateslices == true){

        for ( unsigned int i = 0; i < m_SegmentationIndices.size()-1; i++ ){ // Need to extract intermediate slice

            const int numSlices =  m_SegmentationIndices[i+1] -  m_SegmentationIndices[i];
            int intermediate_slice = numSlices/2;

            typename TInputImage::IndexType regionIndex;
            regionIndex[0] = bbox_index[0] + m_SegmentationIndices[i] + intermediate_slice -1;
            regionIndex[1] = bbox_index[1];
            regionIndex[2] = bbox_index[2];

            typename TInputImage::RegionType slice_region(regionIndex, regionSize);

            // Add all the images on the stack to the filter
            filter->AddScalarImage(intensity_image);

            // Pass the classifier to the filter
            filter->SetClassifier(classifier);

            // Set the filter behavior
            filter->SetGenerateClassProbabilities(true);

            filter->GetOutput()->SetRequestedRegion(slice_region);

            // Run the filter for this set of weights
            filter->Update();

            typename TInputImage::Pointer RFprobability = filter->GetOutput(1);
            RFprobability->DisconnectPipeline();

            // Copy the probability map to the original image space
            itk::ImageSliceConstIteratorWithIndex<TInputImage> It_source( RFprobability, RFprobability->GetRequestedRegion() );
            It_source.SetFirstDirection(1);
            It_source.SetSecondDirection(2);
            It_source.GoToBegin();

            itk::ImageSliceConstIteratorWithIndex<TInputImage> It_destination( output, output->GetLargestPossibleRegion() );
            It_destination.SetFirstDirection(1);
            It_destination.SetSecondDirection(2);
            std::cout << "Initial index RF copying" << It_destination.GetIndex() << std::endl;

            double pixel_val;
            typename TInputImage::IndexType idx;
            typename TInputImage::IndexType source_idx;

            while( !It_source.IsAtEnd() )
            {
                while( !It_source.IsAtEndOfSlice() )
                {

                    while( !It_source.IsAtEndOfLine() )
                    {

                        idx  = It_destination.GetIndex();
                        idx[0] = idx[0] +  bbox_index[0] + m_SegmentationIndices[i] + intermediate_slice -1;
                        idx[1] = idx[1] + bbox_index[1] -1;
                        idx[2] = idx[2] + bbox_index[2] -1;

                        source_idx = It_source.GetIndex();

                        pixel_val = RFprobability->GetPixel(source_idx);
                        output->SetPixel(idx, pixel_val);

                        ++It_source;
                        ++It_destination;
                    }
                    It_source.NextLine();
                    It_destination.NextLine();
                }
                It_source.NextSlice();
                It_destination.NextSlice();
                std::cout << idx << std::endl;
            }

        } // end of slice iterator

    }
    else{
        //Loop through all the slices not contained in m_SegmentationIndices;
        std::vector<int> sliceRange;
        for( int i = 1; i <= bbox_size[m_slicingaxis]; i++ )
            sliceRange.push_back( i );

        std::vector<int> noSegmentations;

         std::set_difference(sliceRange.begin(), sliceRange.end(), m_SegmentationIndices.begin(), m_SegmentationIndices.end(),
                             std::inserter(noSegmentations, noSegmentations.begin()));

        for ( unsigned int i = 0; i < noSegmentations.size(); i++ ){

            int slice_index = noSegmentations[i];

            typename TInputImage::IndexType regionIndex;
            regionIndex[0] = bbox_index[0] + slice_index -1;
            regionIndex[1] = bbox_index[1];
            regionIndex[2] = bbox_index[2];

            typename TInputImage::RegionType slice_region(regionIndex, regionSize);

            // Add all the images on the stack to the filter
            filter->AddScalarImage(intensity_image);

            // Pass the classifier to the filter
            filter->SetClassifier(classifier);

            // Set the filter behavior
            filter->SetGenerateClassProbabilities(true);

            filter->GetOutput()->SetRequestedRegion(slice_region);

            // Run the filter for this set of weights
            filter->Update();

            typename TInputImage::Pointer RFprobability = filter->GetOutput(1);
            RFprobability->DisconnectPipeline();

            // Copy the probability map to the original image space

            itk::ImageSliceConstIteratorWithIndex<TInputImage> It_source( RFprobability, RFprobability->GetRequestedRegion() );
            It_source.SetFirstDirection(1);
            It_source.SetSecondDirection(2);
            It_source.GoToBegin();

            itk::ImageSliceConstIteratorWithIndex<TInputImage> It_destination( output, output->GetLargestPossibleRegion() );
            It_destination.SetFirstDirection(1);
            It_destination.SetSecondDirection(2);

            double pixel_val;
            typename TInputImage::IndexType idx;
            typename TInputImage::IndexType source_idx;

            while( !It_source.IsAtEnd() )
            {
                while( !It_source.IsAtEndOfSlice() )
                {

                    while( !It_source.IsAtEndOfLine() )
                    {

                        idx  = It_destination.GetIndex();
                        idx[0] = idx[0] +  bbox_index[0] + slice_index -1;
                        idx[1] = idx[1] + bbox_index[1] -1;
                        idx[2] = idx[2] + bbox_index[2] -1;
                        source_idx = It_source.GetIndex();

                        pixel_val = RFprobability->GetPixel(source_idx);
                        output->SetPixel(idx, pixel_val);

                        ++It_source;
                        ++It_destination;
                    }
                    It_source.NextLine();
                    It_destination.NextLine();
                }
                It_source.NextSlice();
                It_destination.NextSlice();
            }

        } // end of slice iterator

    } // end of intermediateslices = false

} //End of GenerateData

}// end namespace

#endif

// Create a classifier object - Can be removed
//    typedef typename FilterType::ClassifierType ApplyRFClassifierType;
//    typename ApplyRFClassifierType::Pointer applyclassifier = ApplyRFClassifierType::New();

//    // Read the classifier object from disk
//    std::ifstream in_file(train_file);
//    applyclassifier->Read(in_file);
//    in_file.close();
