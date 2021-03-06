#include "keyframe.h"

KeyFrame::KeyFrame(double _header, int _global_index, Eigen::Vector3d _T_w_i, Eigen::Matrix3d _R_w_i, 
                    cv::Mat &_image, const char *_brief_pattern_file)
:header{_header}, global_index{_global_index}, image{_image}, BRIEF_PATTERN_FILE(_brief_pattern_file)
{
    T_w_i = _T_w_i;
    R_w_i = _R_w_i;
    COL = image.cols;
    ROW = image.rows;
    use_retrive = false;
    is_looped = 0;
    has_loop = 0;
    update_loop_info = 0;
    origin_T_w_i = _T_w_i;
    origin_R_w_i = _R_w_i;
    check_loop = 0;
}

/*****************************************utility function************************************************/
bool inBorder(const cv::Point2f &pt, int COL, int ROW)
{
    const int BORDER_SIZE = 1;
    int img_x = cvRound(pt.x);
    int img_y = cvRound(pt.y);
    return BORDER_SIZE <= img_x && img_x < COL - BORDER_SIZE && BORDER_SIZE <= img_y && img_y < ROW - BORDER_SIZE;
}

template <typename Derived>
static void reduceVector(vector<Derived> &v, vector<uchar> status)
{
    int j = 0;
    for (int i = 0; i < int(v.size()); i++)
        if (status[i])
            v[j++] = v[i];
    v.resize(j);
}

void KeyFrame::rejectWithF(vector<cv::Point2f> &measurements_old,
                 vector<cv::Point2f> &measurements_old_norm,
                 const camodocal::CameraPtr &m_camera)
{
    if (measurements_old.size() >= 8)
    {
        measurements_old_norm.clear();

        vector<cv::Point2f> un_measurements(measurements.size()), un_measurements_old(measurements_old.size());
        for (int i = 0; i < (int)measurements.size(); i++)
        {
            double FOCAL_LENGTH = 460.0;
            Eigen::Vector3d tmp_p;
            m_camera->liftProjective(Eigen::Vector2d(measurements[i].x, measurements[i].y), tmp_p);
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_measurements[i] = cv::Point2f(tmp_p.x(), tmp_p.y());

            m_camera->liftProjective(Eigen::Vector2d(measurements_old[i].x, measurements_old[i].y), tmp_p);
            measurements_old_norm.push_back(cv::Point2f(tmp_p.x()/tmp_p.z(), tmp_p.y()/tmp_p.z()));
            tmp_p.x() = FOCAL_LENGTH * tmp_p.x() / tmp_p.z() + COL / 2.0;
            tmp_p.y() = FOCAL_LENGTH * tmp_p.y() / tmp_p.z() + ROW / 2.0;
            un_measurements_old[i] = cv::Point2f(tmp_p.x(), tmp_p.y());
        }

        vector<uchar> status;
        cv::findFundamentalMat(un_measurements, un_measurements_old, cv::FM_RANSAC, 1.0, 0.99, status);
        reduceVector(point_clouds, status);
        reduceVector(measurements, status);
        reduceVector(measurements_old, status);
        reduceVector(measurements_old_norm, status);
        reduceVector(features_id, status);
    }
}
/*****************************************utility function************************************************/

void KeyFrame::extractBrief(cv::Mat &image)
{
    BriefExtractor extractor(BRIEF_PATTERN_FILE);
    extractor(image, measurements, keypoints, descriptors);
    int start = keypoints.size() - measurements.size();
    for(int i = 0; i< (int)measurements.size(); i++)
    {
        window_keypoints.push_back(keypoints[start + i]);
        window_descriptors.push_back(descriptors[start + i]);
    }
}
void KeyFrame::setExtrinsic(Eigen::Vector3d T, Eigen::Matrix3d R)
{
    qic = R;
    tic = T;
}

void KeyFrame::buildKeyFrameFeatures(Estimator &estimator, const camodocal::CameraPtr &m_camera)
{
    for (auto &it_per_id : estimator.f_manager.feature)
    {
        it_per_id.used_num = it_per_id.feature_per_frame.size();
        //if (!(it_per_id.used_num >= 2 && it_per_id.start_frame < WINDOW_SIZE - 2))
        //    continue;

        int frame_size = it_per_id.feature_per_frame.size();
        if(it_per_id.start_frame <= WINDOW_SIZE - 2 && it_per_id.start_frame + frame_size - 1 >= WINDOW_SIZE - 2)
        {
            //features current measurements
            Vector3d point = it_per_id.feature_per_frame[WINDOW_SIZE - 2 - it_per_id. start_frame].point;
            Vector2d point_uv;
            m_camera->spaceToPlane(point, point_uv);
            measurements.push_back(cv::Point2f(point_uv.x(), point_uv.y()));
            pts_normalize.push_back(cv::Point2f(point.x()/point.z(), point.y()/point.z()));
            
            features_id.push_back(it_per_id.feature_id);
            //features 3D pos from first measurement and inverse depth
            Vector3d pts_i = it_per_id.feature_per_frame[0].point * it_per_id.estimated_depth;
            point_clouds.push_back(estimator.Rs[it_per_id.start_frame] * (qic * pts_i + tic) + estimator.Ps[it_per_id.start_frame]);
        }
    }
    measurements_origin  = measurements;
    point_clouds_origin = point_clouds;
    features_id_origin = features_id;
}

/**
** search matches by guide descriptor match
**
**/
bool KeyFrame::inAera(cv::Point2f pt, cv::Point2f center, float area_size)
{
    if(center.x < 0 || center.x > COL || center.y < 0 || center.y > ROW)
        return false;
    if(pt.x > center.x - area_size && pt.x < center.x + area_size &&
       pt.y > center.y - area_size && pt.y < center.y + area_size)
        return true;
    else
        return false;
}

bool KeyFrame::searchInAera(cv::Point2f center_cur, float area_size,
                            const BRIEF::bitset window_descriptor,
                            const std::vector<BRIEF::bitset> &descriptors_old,
                            const std::vector<cv::KeyPoint> &keypoints_old,
                            cv::Point2f &best_match)
{
    cv::Point2f best_pt;
    int bestDist = 128;
    int bestIndex = -1;
    for(int i = 0; i < (int)descriptors_old.size(); i++)
    {
        if(!inAera(keypoints_old[i].pt, center_cur, area_size))
            continue;

        int dis = HammingDis(window_descriptor, descriptors_old[i]);
        if(dis < bestDist)
        {
            bestDist = dis;
            bestIndex = i;
        }
    }
    if (bestIndex != -1)
    {
      best_match = keypoints_old[bestIndex].pt;
      return true;
    }
    else
      return false;
}

void KeyFrame::searchByDes(const Eigen::Vector3d T_w_i_old, const Eigen::Matrix3d R_w_i_old,
                           std::vector<cv::Point2f> &measurements_old, 
                           std::vector<cv::Point2f> &measurements_old_norm,
                           const std::vector<BRIEF::bitset> &descriptors_old,
                           const std::vector<cv::KeyPoint> &keypoints_old,
                           const camodocal::CameraPtr &m_camera)
{
    //ROS_INFO("loop_match before cur %d %d, old %d", (int)window_descriptors.size(), (int)measurements.size(), (int)descriptors_old.size());
    std::vector<uchar> status;
    for(int i = 0; i < (int)window_descriptors.size(); i++)
    {
        cv::Point2f pt(0.f, 0.f);
        if (searchInAera(measurements[i], 200, window_descriptors[i], descriptors_old, keypoints_old, pt))
          status.push_back(1);
        else
          status.push_back(0);
        measurements_old.push_back(pt);
    }
    reduceVector(measurements, status);
    reduceVector(measurements_old, status);
    reduceVector(features_id, status);
    reduceVector(point_clouds, status);
    reduceVector(measurements_old_norm, status);

    rejectWithF(measurements_old, measurements_old_norm, m_camera);
    //rejectWithF(measurements_old, measurements_old_norm, m_camera); 
    //ROS_INFO("loop_match after cur %d %d, old %d\n", (int)window_descriptors.size(), (int)measurements.size(), (int)descriptors_old.size());
}

/**
*** interface to VINS
*** input: looped old keyframe which include image and pose, and feature correnspondance given by BoW
*** output: ordered old feature correspondance with current KeyFrame and the translation drift
**/
bool KeyFrame::findConnectionWithOldFrame(const KeyFrame* old_kf,
                                          const std::vector<cv::Point2f> &cur_pts, const std::vector<cv::Point2f> &old_pts,
                                          std::vector<cv::Point2f> &measurements_old, std::vector<cv::Point2f> &measurements_old_norm,
                                          const camodocal::CameraPtr &m_camera)
{
    TicToc t_match;
    searchByDes(old_kf->T_w_i, old_kf->R_w_i, measurements_old, measurements_old_norm, old_kf->descriptors, old_kf->keypoints, m_camera);
    ROS_DEBUG("loop final use num %d %lf---------------", (int)measurements_old.size(), t_match.toc());
    
    return true;
}

void KeyFrame::updatePose(const Eigen::Vector3d &_T_w_i, const Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    T_w_i = _T_w_i;
    R_w_i = _R_w_i;
}

void KeyFrame::updateOriginPose(const Eigen::Vector3d &_T_w_i, const Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    origin_T_w_i = _T_w_i;
    origin_R_w_i = _R_w_i;
}

void KeyFrame::getPose(Eigen::Vector3d &_T_w_i, Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    _T_w_i = T_w_i;
    _R_w_i = R_w_i;
}

void KeyFrame::getOriginPose(Eigen::Vector3d &_T_w_i, Eigen::Matrix3d &_R_w_i)
{
    unique_lock<mutex> lock(mMutexPose);
    _T_w_i = origin_T_w_i;
    _R_w_i = origin_R_w_i;
}

void KeyFrame::updateLoopConnection(Vector3d relative_t, Quaterniond relative_q, double relative_yaw)
{
    has_loop = 1;
    update_loop_info = 1;
    unique_lock<mutex> lock(mLoopInfo);
    Eigen::Matrix<double, 8, 1> connected_info;
    connected_info <<relative_t.x(), relative_t.y(), relative_t.z(),
                     relative_q.w(), relative_q.x(), relative_q.y(), relative_q.z(),
                     relative_yaw;
    loop_info = connected_info;
}

Eigen::Vector3d KeyFrame::getLoopRelativeT()
{
    assert(update_loop_info == 1);
    unique_lock<mutex> lock(mLoopInfo);
    return Eigen::Vector3d(loop_info(0), loop_info(1), loop_info(2));
}

double KeyFrame::getLoopRelativeYaw()
{
    assert(update_loop_info == 1);
    unique_lock<mutex> lock(mLoopInfo);
    return loop_info(7);
}

void KeyFrame::detectLoop(int index)
{
    has_loop = true;
    loop_index = index;
}

void KeyFrame::removeLoop()
{
    has_loop = false;
    update_loop_info = 0;
}

int KeyFrame::HammingDis(const BRIEF::bitset &a, const BRIEF::bitset &b)
{
    BRIEF::bitset xor_of_bitset = a ^ b;
    int dis = xor_of_bitset.count();
    return dis;
}

BriefExtractor::BriefExtractor(const std::string &pattern_file)
{
  // The DVision::BRIEF extractor computes a random pattern by default when
  // the object is created.
  // We load the pattern that we used to build the vocabulary, to make
  // the descriptors compatible with the predefined vocabulary
  
  // loads the pattern
  cv::FileStorage fs(pattern_file.c_str(), cv::FileStorage::READ);
  if(!fs.isOpened()) throw string("Could not open file ") + pattern_file;
  
  vector<int> x1, y1, x2, y2;
  fs["x1"] >> x1;
  fs["x2"] >> x2;
  fs["y1"] >> y1;
  fs["y2"] >> y2;
  
  m_brief.importPairs(x1, y1, x2, y2);
}

void BriefExtractor::operator() (const cv::Mat &im, const std::vector<cv::Point2f> window_pts,
  vector<cv::KeyPoint> &keys, vector<BRIEF::bitset> &descriptors) const
{
  // extract FAST keypoints with opencv
  const int fast_th = 20; // corner detector response threshold
  cv::FAST(im, keys, fast_th, true);
  for(int i = 0; i < (int)window_pts.size(); i++)
  {
      cv::KeyPoint key;
      key.pt = window_pts[i];
      keys.push_back(key);
  }
  // compute their BRIEF descriptor
  m_brief.compute(im, keys, descriptors);
}