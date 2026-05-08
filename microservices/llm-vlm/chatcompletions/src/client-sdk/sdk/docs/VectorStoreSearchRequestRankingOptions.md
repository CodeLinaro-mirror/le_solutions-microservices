# VectorStoreSearchRequestRankingOptions

Ranking options for search.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**ranker** | **str** |  | [optional] [default to 'auto']
**score_threshold** | **float** |  | [optional] [default to 0]

## Example

```python
from openapi_client.models.vector_store_search_request_ranking_options import VectorStoreSearchRequestRankingOptions

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreSearchRequestRankingOptions from a JSON string
vector_store_search_request_ranking_options_instance = VectorStoreSearchRequestRankingOptions.from_json(json)
# print the JSON string representation of the object
print(VectorStoreSearchRequestRankingOptions.to_json())

# convert the object into a dict
vector_store_search_request_ranking_options_dict = vector_store_search_request_ranking_options_instance.to_dict()
# create an instance of VectorStoreSearchRequestRankingOptions from a dict
vector_store_search_request_ranking_options_from_dict = VectorStoreSearchRequestRankingOptions.from_dict(vector_store_search_request_ranking_options_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


